#!/usr/bin/env python3
"""Kama-HTTPServer inference benchmark.

Measures P50/P95/P99 latency, QPS, and model-level timings by calling
POST /predict and GET /metrics/json repeatedly.

Usage:
    python3 scripts/benchmark.py [--requests N] [--concurrency C] [--image PATH]
"""

import argparse, base64, json, os, statistics, sys, time, urllib.request, urllib.error
from concurrent.futures import ThreadPoolExecutor, as_completed

BASE_URL = os.environ.get("BENCH_URL", "http://localhost:80")
MODEL_NAME = os.environ.get("BENCH_MODEL", "")

def fetch_models() -> list:
    """Return list of available models from GET /models."""
    with urllib.request.urlopen(f"{BASE_URL}/models") as resp:
        return json.loads(resp.read())

def pick_model(models: list) -> str:
    """Pick the first ONNX model, or fall back to first model."""
    for m in models:
        if m.get("type") == "onnx":
            name = m["name"]
            ver = m.get("version", "1")
            return f"{name}:{ver}"
    if models:
        m = models[0]
        return f"{m['name']}:{m.get('version', '1')}"
    return "resnet50"

def run_predict(model: str, image_path: str = "images/cat.jpg") -> tuple:
    """Send one POST /predict request. Uses image_path if file exists, else falls back
    to a pre-encoded base64 image for models/ and images/ directories."""
    payload = json.dumps({"model_name": model, "image_path": image_path}).encode()
    req = urllib.request.Request(f"{BASE_URL}/predict", data=payload,
                                 headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        resp = urllib.request.urlopen(req, timeout=30)
        body = resp.read()
        elapsed = (time.perf_counter() - t0) * 1_000_000
        data = json.loads(body) if body else {}
        ok = data.get("status") == "ok"
        if not ok and ("failed to load image" in data.get("message", "")):
            # Fall back to base64 mode
            return run_predict_base64(model, image_path)
        return elapsed, ok, resp.status
    except urllib.error.HTTPError as e:
        elapsed = (time.perf_counter() - t0) * 1_000_000
        return elapsed, False, e.code
    except Exception as e:
        elapsed = (time.perf_counter() - t0) * 1_000_000
        return elapsed, False, 0


def run_predict_base64(model: str, image_path: str = "images/cat.jpg") -> tuple:
    """Fallback: encode image from filesystem and send as image_data."""
    try:
        with open(image_path, "rb") as f:
            b64 = base64.b64encode(f.read()).decode()
    except Exception:
        return 0, False, 0
    payload = json.dumps({"model_name": model, "image_data": b64}).encode()
    req = urllib.request.Request(f"{BASE_URL}/predict", data=payload,
                                 headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        resp = urllib.request.urlopen(req, timeout=30)
        body = resp.read()
        elapsed = (time.perf_counter() - t0) * 1_000_000
        data = json.loads(body) if body else {}
        return elapsed, data.get("status") == "ok", resp.status
    except urllib.error.HTTPError as e:
        elapsed = (time.perf_counter() - t0) * 1_000_000
        return elapsed, False, e.code
    except Exception as e:
        elapsed = (time.perf_counter() - t0) * 1_000_000
        return elapsed, False, 0

def fetch_metrics() -> dict:
    """Fetch /metrics/json and return parsed dict."""
    with urllib.request.urlopen(f"{BASE_URL}/metrics/json") as resp:
        return json.loads(resp.read())

def print_header(title: str):
    print(f"\n{'='*60}\n  {title}\n{'='*60}")

def main():
    parser = argparse.ArgumentParser(description="Kama-HTTPServer benchmark")
    parser.add_argument("-n", "--requests", type=int, default=100,
                        help="Total number of requests (default: 100)")
    parser.add_argument("-c", "--concurrency", type=int, default=4,
                        help="Number of concurrent workers (default: 4)")
    parser.add_argument("--image", type=str, default="images/cat.jpg",
                        help="Image path for predict (default: images/cat.jpg)")
    parser.add_argument("--model", type=str, default="",
                        help="Model name:version (default: auto-detect first ONNX)")
    parser.add_argument("--warmup", type=int, default=5,
                        help="Warmup requests before measuring (default: 5)")
    args = parser.parse_args()

    # Discover model
    global MODEL_NAME
    if args.model:
        MODEL_NAME = args.model
    else:
        try:
            models = fetch_models()
            MODEL_NAME = pick_model(models)
            print(f"Auto-detected model: {MODEL_NAME} "
                  f"(from {len(models)} loaded models)")
        except Exception as e:
            print(f"Failed to fetch models: {e}")
            MODEL_NAME = "resnet50"

    # Health check
    try:
        with urllib.request.urlopen(f"{BASE_URL}/health") as resp:
            h = json.loads(resp.read())
            print(f"Health check: {h.get('status', '?')}")
    except Exception as e:
        print(f"ERROR: Cannot reach {BASE_URL}/health — {e}")
        sys.exit(1)

    # Print pre-benchmark metrics
    print_header("PRE-BENCHMARK METRICS")
    try:
        pre = fetch_metrics()
        uptime = pre.get("uptime_seconds", 0)
        eps = pre.get("endpoints", {})
        predict_ep = eps.get("POST:/predict", {})
        print(f"  Uptime: {uptime:.0f}s")
        print(f"  /predict requests so far: {predict_ep.get('total', 0)}")
        if predict_ep.get("total", 0) > 0:
            print(f"  /predict avg latency: {predict_ep.get('avg_latency_us', 0)} µs")
    except Exception as e:
        print(f"  Failed: {e}")

    # Warmup
    print_header(f"WARMUP ({args.warmup} requests)")
    for i in range(args.warmup):
        lat, ok, code = run_predict(MODEL_NAME, args.image)
        print(f"  warmup {i+1}/{args.warmup}: {lat:.0f} µs, "
              f"{'OK' if ok else 'FAIL'} ({code})")

    # Benchmark
    print_header(f"BENCHMARK ({args.requests} requests, {args.concurrency} concurrent)")
    latencies = []
    errors = 0
    t_start = time.perf_counter()

    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futures = [pool.submit(run_predict, MODEL_NAME, args.image)
                   for _ in range(args.requests)]
        for f in as_completed(futures):
            lat, ok, code = f.result()
            latencies.append(lat)
            if not ok:
                errors += 1

    t_total = time.perf_counter() - t_start

    # Calculate statistics
    latencies.sort()
    n = len(latencies)
    p50 = latencies[n // 2] if n > 0 else 0
    p95 = latencies[int(n * 0.95)] if n > 1 else latencies[0] if n > 0 else 0
    p99 = latencies[int(n * 0.99)] if n > 2 else latencies[-1] if n > 0 else 0
    avg = statistics.mean(latencies) if latencies else 0
    qps = args.requests / t_total if t_total > 0 else 0

    print(f"  Total time:  {t_total:.2f} s")
    print(f"  Requests:    {args.requests} ({errors} errors)")
    print(f"  QPS:         {qps:.1f}")
    print(f"  Latency (µs):")
    print(f"    avg:       {avg:.0f}")
    print(f"    P50:       {p50:.0f}")
    print(f"    P95:       {p95:.0f}")
    print(f"    P99:       {p99:.0f}")
    print(f"    min:       {latencies[0] if latencies else 0:.0f}")
    print(f"    max:       {latencies[-1] if latencies else 0:.0f}")

    # Post-benchmark metrics
    print_header("POST-BENCHMARK METRICS")
    try:
        post = fetch_metrics()
        predict_ep = post.get("endpoints", {}).get("POST:/predict", {})
        total = predict_ep.get("total", 0)
        avg_lat = predict_ep.get("avg_latency_us", 0)
        buckets = predict_ep.get("buckets", {})
        print(f"  /predict total (lifetime): {total}")
        print(f"  /predict avg (lifetime):   {avg_lat} µs")
        print(f"  /predict latency buckets:")
        for b, v in buckets.items():
            print(f"    {b}: {v}")

        # Model-level metrics
        model_metrics = post.get("model_inference", {})
        if model_metrics:
            print(f"\n  Model inference breakdown:")
            for key, mm in model_metrics.items():
                print(f"    {key}: count={mm['count']}, "
                      f"avg={mm['avg_latency_us']} µs, "
                      f"min={mm['latency_us_min']} µs, "
                      f"max={mm['latency_us_max']} µs")
                for b, v in mm.get("buckets", {}).items():
                    if v > 0:
                        print(f"      {b}: {v}")
    except Exception as e:
        print(f"  Failed: {e}")

    print_header("DONE")
    # Write baseline report
    report_file = f"pressureresult-baseline-{time.strftime('%Y%m%d-%H%M%S')}.md"
    try:
        with open(report_file, "w") as f:
            f.write(f"# Kama-HTTPServer Benchmark Baseline\n\n"
                    f"**Date:** {time.strftime('%Y-%m-%d %H:%M:%S')}\n"
                    f"**Model:** {MODEL_NAME}\n"
                    f"**Requests:** {args.requests} (concurrency={args.concurrency})\n\n"
                    f"## Results\n\n"
                    f"| Metric | Value |\n|--------|-------|\n"
                    f"| QPS | {qps:.1f} |\n"
                    f"| Avg latency | {avg:.0f} µs |\n"
                    f"| P50 | {p50:.0f} µs |\n"
                    f"| P95 | {p95:.0f} µs |\n"
                    f"| P99 | {p99:.0f} µs |\n"
                    f"| Errors | {errors} |\n"
                    f"| Total time | {t_total:.2f} s |\n")
        print(f"  Report saved: {report_file}")
    except Exception as e:
        print(f"  Failed to write report: {e}")

if __name__ == "__main__":
    main()
