#!/usr/bin/env python3
"""极限 QPS 压测 — 并发阶梯探顶

Usage:
  python3 scripts/quick_maxqps.py
"""

import argparse, json, sys, time, urllib.request, urllib.error
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# Windows proxy auto-detection (WPAD/PAC) adds ~21s delay per connection.
# Disable it globally for all urllib operations in this script.
_NO_PROXY_HANDLER = urllib.request.ProxyHandler({})
_OPENER = urllib.request.build_opener(_NO_PROXY_HANDLER)
urllib.request.install_opener(_OPENER)

# ── Config ──────────────────────────────────────────────────────────
BASE_URL  = "http://127.0.0.1:80"  # 127.0.0.1 avoids Windows IPv6 getaddrinfo 21s delay
MODEL     = "squeezenet1.1-7_trt:1"
IMAGE_DIR = Path(__file__).resolve().parent.parent / "images"

# 并发阶梯
CONCURRENCIES = [1, 2, 4, 8, 16, 32, 64, 128, 256]
DURATION_SEC  = 10   # 每级持续时间


def load_images_raw():
    """Load all test images as raw JPEG/PNG bytes."""
    images = []
    for ext in ("*.jpg", "*.jpeg", "*.png"):
        for p in sorted(IMAGE_DIR.glob(ext)):
            with open(p, "rb") as f:
                images.append((p.name, f.read()))
    if not images:
        sys.exit(f"ERROR: No images found in {IMAGE_DIR}")
    print(f"  Loaded {len(images)} raw images from {IMAGE_DIR}")
    return images


def predict_once(model_name, img_raw):
    """Send one prediction via /predict/raw (binary JPEG body).
    Returns (latency_us, ok, http_status)."""
    url = f"{BASE_URL}/predict/raw?model_name={model_name}"
    req = urllib.request.Request(url, data=img_raw,
                                  headers={"Content-Type": "image/jpeg"})
    t0 = time.perf_counter()
    try:
        resp = urllib.request.urlopen(req, timeout=30)
        body = resp.read().decode()
        elapsed = (time.perf_counter() - t0) * 1_000_000
        data = json.loads(body) if body else {}
        ok = (data.get("status") == "ok" or data.get("status") == "success")
        return elapsed, ok, resp.status
    except urllib.error.HTTPError as e:
        body = e.read().decode() if e.fp else ""
        elapsed = (time.perf_counter() - t0) * 1_000_000
        return elapsed, False, e.code
    except Exception as e:
        return (time.perf_counter() - t0) * 1_000_000, False, 0


def run_ramp(images, model_name):
    """Run concurrency ramp test, returns list of results per level."""
    print(f"\n{'='*60}")
    print(f"  LIMIT QPS RAMP TEST")
    print(f"  Model: {model_name}  |  Endpoint: /predict/raw")
    print(f"  Duration: {DURATION_SEC}s/level")
    print(f"{'='*60}")
    print(f"{'Conc':>5} | {'QPS':>8} | {'P50':>8} | {'P95':>8} | {'P99':>8} | {'Avg':>8} | {'Err%':>6}")
    print("-" * 75)

    results = []
    peak_qps = 0
    peak_conc = 0

    for c in CONCURRENCIES:
        # Warmup — 5 quick requests
        for i in range(5):
            predict_once(model_name, images[i % len(images)][1])

        latencies = []
        errors = 0

        def worker():
            _, img_bytes = images[int(time.time() * 1000) % len(images)]
            return predict_once(model_name, img_bytes)

        with ThreadPoolExecutor(max_workers=c) as pool:
            futures = {}
            t_start = time.perf_counter()
            end_time = t_start + DURATION_SEC
            # Keep pool saturated: maintain ~2x concurrency futures pending
            while time.perf_counter() < end_time:
                # Top up futures
                while len(futures) < c * 2:
                    f = pool.submit(worker)
                    futures[f] = time.perf_counter()

                # Collect done futures
                done = set()
                for f in futures:
                    if f.done():
                        done.add(f)
                for f in done:
                    futures.pop(f)
                    try:
                        lat, ok, code = f.result()
                        if ok:
                            latencies.append(lat)
                        else:
                            errors += 1
                    except Exception:
                        errors += 1
                time.sleep(0.0005)  # 0.5ms poll interval

            t_window_end = time.perf_counter()
            window_duration = t_window_end - t_start  # actual test window (excl drain)

            # Drain remaining in-flight futures (don't lose tail requests)
            for f in list(futures):
                try:
                    lat, ok, code = f.result(timeout=30)
                    if ok:
                        latencies.append(lat)
                    else:
                        errors += 1
                except Exception:
                    errors += 1

        n = len(latencies)
        total_req = n + errors
        qps = total_req / window_duration if window_duration > 0 else 0

        if n == 0:
            print(f"  {c:>5} | {'—':>8} | {'—':>8} | {'—':>8} | {'—':>8} | {'—':>8} | {100*errors/max(1,total_req):>5.1f}%")
            results.append({"concurrency": c, "qps": 0, "errors_pct": 100})
            break

        latencies.sort()
        p50 = latencies[n // 2] / 1000
        p95 = latencies[int(n * 0.95)] / 1000 if n > 1 else p50
        p99 = latencies[int(n * 0.99)] / 1000 if n > 2 else latencies[-1] / 1000
        avg = sum(latencies) / n / 1000
        err_pct = 100 * errors / max(1, total_req)

        print(f"  {c:>5} | {qps:>8.1f} | {p50:>7.1f}ms | {p95:>7.1f}ms | {p99:>7.1f}ms | {avg:>7.1f}ms | {err_pct:>5.1f}%")

        results.append({
            "concurrency": c,
            "qps": round(qps, 1),
            "p50_ms": round(p50, 1),
            "p95_ms": round(p95, 1),
            "p99_ms": round(p99, 1),
            "avg_ms": round(avg, 1),
            "errors_pct": round(err_pct, 1),
        })

        # Update peak
        if qps > peak_qps:
            peak_qps = qps
            peak_conc = c

        # Saturation detection
        if len(results) >= 3:
            prev = results[-2]["qps"]
            if prev > 0:
                delta = abs(qps - prev) / prev
                if delta < 0.03 and c >= 8:
                    print(f"  → Saturated: QPS plateau at conc={c} (delta={delta*100:.1f}%)")
                    break

        # Error threshold check
        if err_pct > 10:
            print(f"  → Stopping: error rate > 10%")
            break

        # P99 latency blowup
        if p99 > 500:
            print(f"  → P99 > 500ms, system overloaded")
            break

    print(f"\n  Peak: {peak_qps:.1f} req/s @ concurrency={peak_conc}")
    return results


def main():
    global DURATION_SEC, CONCURRENCIES

    parser = argparse.ArgumentParser(description="Limit QPS benchmark")
    parser.add_argument("--conc", type=int, nargs="+",
                        help="Override concurrency levels (e.g. --conc 16 32 64)")
    parser.add_argument("--duration", type=int, default=None,
                        help=f"Duration per level (default {DURATION_SEC}s)")
    parser.add_argument("--model", default=MODEL,
                        help=f"Model name (default: {MODEL})")
    args = parser.parse_args()

    if args.duration is not None:
        DURATION_SEC = args.duration
    if args.conc:
        CONCURRENCIES = args.conc

    # Health check
    print(f"Target: {BASE_URL}")
    try:
        r = urllib.request.urlopen(f"{BASE_URL}/health", timeout=5)
        payload = json.loads(r.read().decode())
        if payload.get("status") != "ok":
            sys.exit(f"Health check failed: {payload}")
        print("Health: OK")
    except Exception as e:
        sys.exit(f"Cannot reach {BASE_URL}: {e}")

    # Check model is loaded
    try:
        models = json.loads(urllib.request.urlopen(
            f"{BASE_URL}/models", timeout=10).read().decode())
        model_names = {m["name"] for m in models}
        # strip version suffix for checking
        model_base = args.model.rsplit(":", 1)[0]
        if model_base not in model_names:
            print(f"  WARNING: model '{model_base}' not found in loaded models: {model_names}")
            print(f"  Attempting to continue anyway...")
        else:
            print(f"Model '{model_base}': loaded ✓")
    except Exception as e:
        print(f"  WARNING: Could not check models: {e}")

    print(f"Model: {args.model}")
    print(f"Concurrency levels: {CONCURRENCIES}")
    print(f"Duration per level: {DURATION_SEC}s")

    images = load_images_raw()
    results = run_ramp(images, args.model)

    # Final summary
    if results:
        valid = [r for r in results if r["qps"] > 0]
        if valid:
            best = max(valid, key=lambda r: r["qps"])
            print(f"\n{'='*60}")
            print(f"  LIMIT QPS: {best['qps']} req/s")
            print(f"  @ concurrency={best['concurrency']}")
            print(f"  P50={best['p50_ms']}ms  P95={best['p95_ms']}ms  P99={best['p99_ms']}ms")
            print(f"  Errors: {best['errors_pct']}%")
            print(f"{'='*60}")


if __name__ == "__main__":
    main()
