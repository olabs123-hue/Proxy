"""Serial latency sampler, not a production throughput benchmark."""
import argparse
import statistics
import time
import urllib.request

def percentile(sorted_values, q):
    return sorted_values[min(len(sorted_values)-1, int((len(sorted_values)-1)*q))]

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--url", default="http://127.0.0.1:6188/bench")
    p.add_argument("--requests", type=int, default=200)
    args = p.parse_args()
    assert args.requests > 0
    values = []
    errors = 0
    for _ in range(args.requests):
        start = time.perf_counter()
        try:
            with urllib.request.urlopen(args.url, timeout=5) as r:
                r.read()
                if r.status != 200:
                    errors += 1
        except Exception:
            errors += 1
        values.append((time.perf_counter() - start) * 1000)
    values.sort()
    print({"requests": args.requests, "errors": errors, "p50_ms": round(percentile(values,.5),3), "p95_ms": round(percentile(values,.95),3), "p99_ms": round(percentile(values,.99),3)})
    print("Serial requests only; use identical hardware/workload for C++ comparison.")

if __name__ == "__main__":
    main()
