#!/usr/bin/env python3
"""Fixed-profile inference benchmark.

This script intentionally does not apply profiles, restart the server, or
load/unload models. Run it after manually selecting the server configuration
you want to test.
"""

import argparse
import http.client
import json
import random
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from urllib.parse import quote


DEFAULT_CONCURRENCIES = [1, 2, 4, 8, 16, 32, 64, 128]


def percentile(sorted_values, pct):
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return sorted_values[0]
    idx = int(len(sorted_values) * pct)
    if idx >= len(sorted_values):
        idx = len(sorted_values) - 1
    return sorted_values[idx]


def load_images(image_dir):
    paths = []
    for pattern in ("*.jpg", "*.jpeg", "*.png"):
        paths.extend(sorted(image_dir.glob(pattern)))
    images = []
    for path in paths:
        with path.open("rb") as f:
            images.append((path.name, f.read()))
    if not images:
        raise RuntimeError(f"no images found in {image_dir}")
    return images


def request_json(host, port, method, path, body=None, headers=None, timeout=10):
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        data = resp.read()
        if resp.status < 200 or resp.status >= 300:
            raise RuntimeError(f"{method} {path} returned HTTP {resp.status}: {data[:200]!r}")
        return json.loads(data.decode("utf-8"))
    finally:
        conn.close()


def check_ready(host, port):
    data = request_json(host, port, "GET", "/ready")
    if data.get("status") != "ready":
        raise RuntimeError(f"server not ready: {data}")


def check_model_loaded(host, port, model_name):
    bare, _, version = model_name.partition(":")
    models = request_json(host, port, "GET", "/models")
    for model in models:
        if model.get("name") != bare:
            continue
        if version and str(model.get("version")) != version:
            continue
        return model
    raise RuntimeError(f"model not loaded: {model_name}")


def get_metrics(host, port):
    return request_json(host, port, "GET", "/metrics/json", timeout=15)


def get_nested_number(obj, key, default=0.0):
    value = obj.get(key, default) if isinstance(obj, dict) else default
    try:
        return float(value)
    except (TypeError, ValueError):
        return float(default)


def delta_count(before, after, key):
    return max(0.0, get_nested_number(after, key) - get_nested_number(before, key))


def delta_average(before, after, count_key, avg_key):
    before_count = get_nested_number(before, count_key)
    after_count = get_nested_number(after, count_key)
    d_count = after_count - before_count
    if d_count <= 0:
        return 0.0
    before_sum = get_nested_number(before, avg_key) * before_count
    after_sum = get_nested_number(after, avg_key) * after_count
    return max(0.0, (after_sum - before_sum) / d_count)


def find_phase(metrics, model_bare, task, phase):
    key = f"{model_bare}:{task}:{phase}"
    phases = metrics.get("pipeline_phases", {})
    if key in phases:
        return phases[key]
    suffix = f":{task}:{phase}"
    for phase_key, value in phases.items():
        if phase_key.endswith(suffix) and phase_key.startswith(model_bare):
            return value
    return {}


def extract_server_delta(before, after, model_name, task):
    batching_before = before.get("batching", {}).get(model_name, {})
    batching_after = after.get("batching", {}).get(model_name, {})

    batches = delta_count(batching_before, batching_after, "batches_total")
    requests = delta_count(batching_before, batching_after, "requests_total")
    avg_batch_size = requests / batches if batches > 0 else 0.0

    reasons_before = batching_before.get("dispatch_reasons", {})
    reasons_after = batching_after.get("dispatch_reasons", {})
    timeout = delta_count(reasons_before, reasons_after, "timeout")
    preferred = delta_count(reasons_before, reasons_after, "preferred")
    max_batch = delta_count(reasons_before, reasons_after, "max_batch")

    model_bare = model_name.split(":", 1)[0]
    pre_before = find_phase(before, model_bare, task, "preprocess")
    pre_after = find_phase(after, model_bare, task, "preprocess")
    infer_before = find_phase(before, model_bare, task, "inference")
    infer_after = find_phase(after, model_bare, task, "inference")

    return {
        "batches": batches,
        "requests": requests,
        "avg_batch_size": avg_batch_size,
        "timeout_ratio": timeout / batches if batches > 0 else 0.0,
        "preferred_ratio": preferred / batches if batches > 0 else 0.0,
        "max_batch_ratio": max_batch / batches if batches > 0 else 0.0,
        "preprocess_ms": delta_average(pre_before, pre_after, "count", "avg_latency_us") / 1000.0,
        "gpu_ms": delta_average(infer_before, infer_after, "count", "avg_latency_us") / 1000.0,
        "exec_queue_wait_ms": delta_average(
            batching_before, batching_after, "batches_total", "avg_exec_queue_wait_us"
        ) / 1000.0,
    }


def make_connection(host, port, timeout):
    return http.client.HTTPConnection(host, port, timeout=timeout)


def predict_raw_once(conn, model_name, image_bytes, timeout_path, validate_json):
    headers = {
        "Content-Type": "image/jpeg",
        "Connection": "Keep-Alive",
    }
    start = time.perf_counter()
    try:
        conn.request("POST", timeout_path, body=image_bytes, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        elapsed_us = (time.perf_counter() - start) * 1_000_000.0
        ok = 200 <= resp.status < 300
        if ok and validate_json:
            try:
                ok = json.loads(data.decode("utf-8")).get("status") == "ok"
            except Exception:
                ok = False
        return elapsed_us, ok, resp.status
    except Exception:
        try:
            conn.close()
        except Exception:
            pass
        raise


def worker_loop(worker_id, host, port, model_name, images, duration_sec, validate_json, timeout):
    # The current server query parser does not URL-decode values, so keep ':'
    # literal for versioned model names such as "squeezenet1.1-7_trt:1".
    path = f"/predict/raw?model_name={quote(model_name, safe=':')}"
    conn = make_connection(host, port, timeout)
    rng = random.Random(worker_id * 9973 + int(time.time()))
    deadline = time.perf_counter() + duration_sec
    latencies = []
    errors = 0
    statuses = {}

    while time.perf_counter() < deadline:
        _, image_bytes = images[rng.randrange(len(images))]
        try:
            latency_us, ok, status = predict_raw_once(conn, model_name, image_bytes, path, validate_json)
            statuses[status] = statuses.get(status, 0) + 1
            if ok:
                latencies.append(latency_us)
            else:
                errors += 1
        except Exception:
            errors += 1
            conn = make_connection(host, port, timeout)

    try:
        conn.close()
    except Exception:
        pass
    return latencies, errors, statuses


def run_load(host, port, model_name, images, concurrency, duration_sec, validate_json, timeout):
    latencies = []
    errors = 0
    statuses = {}
    start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [
            pool.submit(worker_loop, i, host, port, model_name, images, duration_sec, validate_json, timeout)
            for i in range(concurrency)
        ]
        for future in as_completed(futures):
            worker_latencies, worker_errors, worker_statuses = future.result()
            latencies.extend(worker_latencies)
            errors += worker_errors
            for status, count in worker_statuses.items():
                statuses[status] = statuses.get(status, 0) + count
    elapsed = time.perf_counter() - start
    return latencies, errors, statuses, elapsed


def format_pct(value):
    return f"{value * 100.0:.0f}"


def run_benchmark(args, images, model_info):
    task = model_info.get("task", args.task)
    concurrencies = [int(x) for x in args.concurrency.split(",") if x.strip()]

    print(f"Target: http://{args.host}:{args.port}")
    print(f"Model: {args.model} task={task}")
    print(f"Images: {len(images)} from {args.image_dir}")
    print(f"Warmup: {args.warmup}s | Duration: {args.duration}s/level")
    print()

    header = (
        f"{'Conc':>5} | {'QPS':>7} | {'P50':>7} | {'P95':>7} | {'P99':>7} | "
        f"{'Err%':>5} | {'BSize':>5} | {'TO%':>4} | {'Pref%':>5} | "
        f"{'PreP':>6} | {'GPU':>5} | {'ExecQ':>6}"
    )
    print(header)
    print("-" * len(header))

    results = []
    for concurrency in concurrencies:
        if args.warmup > 0:
            run_load(
                args.host, args.port, args.model, images, concurrency,
                args.warmup, args.validate_json, args.timeout
            )

        before = get_metrics(args.host, args.port)
        latencies, errors, statuses, elapsed = run_load(
            args.host, args.port, args.model, images, concurrency,
            args.duration, args.validate_json, args.timeout
        )
        after = get_metrics(args.host, args.port)
        server = extract_server_delta(before, after, args.model, task)

        ok_count = len(latencies)
        total_count = ok_count + errors
        latencies.sort()
        qps = ok_count / elapsed if elapsed > 0 else 0.0
        p50 = percentile(latencies, 0.50) / 1000.0
        p95 = percentile(latencies, 0.95) / 1000.0
        p99 = percentile(latencies, 0.99) / 1000.0
        err_pct = (errors / total_count * 100.0) if total_count > 0 else 0.0

        row = {
            "concurrency": concurrency,
            "qps": qps,
            "p50_ms": p50,
            "p95_ms": p95,
            "p99_ms": p99,
            "errors": errors,
            "total": total_count,
            "err_pct": err_pct,
            "statuses": statuses,
            **server,
        }
        results.append(row)

        print(
            f"{concurrency:5d} | {qps:7.1f} | {p50:5.1f}ms | {p95:5.1f}ms | {p99:5.1f}ms | "
            f"{err_pct:4.1f}% | {server['avg_batch_size']:5.1f} | "
            f"{format_pct(server['timeout_ratio']):>3}% | {format_pct(server['preferred_ratio']):>4}% | "
            f"{server['preprocess_ms']:5.1f}ms | {server['gpu_ms']:4.1f}ms | "
            f"{server['exec_queue_wait_ms']:5.1f}ms"
        )

    return results


def main():
    parser = argparse.ArgumentParser(description="Fixed-profile HTTP keep-alive inference benchmark")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--model", default="squeezenet1.1-7_trt:1")
    parser.add_argument("--task", default="classification", help="fallback task for metrics phase keys")
    parser.add_argument("--image-dir", type=Path, default=Path(__file__).resolve().parent.parent / "images")
    parser.add_argument("--concurrency", default="1,2,4,8,16,32,64,128")
    parser.add_argument("--warmup", type=float, default=5.0)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--validate-json", action="store_true", help="parse response JSON and require status=ok")
    parser.add_argument("--json-out", type=Path, help="optional path to write detailed JSON results")
    args = parser.parse_args()

    try:
        images = load_images(args.image_dir)
        check_ready(args.host, args.port)
        model_info = check_model_loaded(args.host, args.port, args.model)
        get_metrics(args.host, args.port)
    except Exception as exc:
        print(f"preflight failed: {exc}", file=sys.stderr)
        return 2

    results = run_benchmark(args, images, model_info)

    if args.json_out:
        payload = {
            "target": f"http://{args.host}:{args.port}",
            "model": args.model,
            "image_dir": str(args.image_dir),
            "warmup_sec": args.warmup,
            "duration_sec": args.duration,
            "results": results,
        }
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        with args.json_out.open("w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2)
        print(f"\nWrote {args.json_out}")

    if results:
        best = max(results, key=lambda item: item["qps"])
        print(
            f"\nBest QPS: conc={best['concurrency']} qps={best['qps']:.1f} "
            f"p95={best['p95_ms']:.1f}ms avg_batch={best['avg_batch_size']:.1f} "
            f"timeout={best['timeout_ratio'] * 100.0:.0f}%"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
