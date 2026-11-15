# EdgeGuard: Rust/Pingora proxy comparison

A separate Rust experiment alongside [my C++17 Linux edge reverse proxy](https://github.com/olabs123-hue/Proxy). This implementation is independent, **not affiliated with Cloudflare**, and based on publicly documented Pingora APIs.

## Features

- Async HTTP reverse proxy based on **Cloudflare Pingora**
- Round-robin routing to configurable local HTTP origins
- Background TCP health checks every second
- Error response rather than a process panic when all backends are unhealthy
- Self-contained Python origin fixtures, smoke tests and serial latency sampler
- CI build and end-to-end traffic-distribution test

## Run

On Linux, install stable Rust/Cargo, pkg-config and OpenSSL development headers.

From this `pingora-rust` directory, open four terminals:

```bash
python3 scripts/origin.py --port 9001 --name origin-a
python3 scripts/origin.py --port 9002 --name origin-b
cargo run
python3 scripts/smoke_test.py --requests 30
```

Run Python unit tests: `python3 -m unittest discover -s tests -v`.
For a basic latency sample run `python3 scripts/benchmark.py --requests 200`.

Configuration:
```bash
EDGEGUARD_LISTEN=127.0.0.1:6188 EDGEGUARD_BACKENDS=127.0.0.1:9001,127.0.0.1:9002 cargo run
```

## Failover exercise

Stop origin A, wait at least 3 seconds for background health checks, then:

```bash
python3 scripts/failover_test.py --expected-origin origin-b
```

Restart origin A, wait for health restoration and repeat the two-origin smoke test.
The transition window can still produce failures; **zero-error failover is not claimed**.

## Scope and honest limitations

This is a portfolio experiment, not a hardened CDN. It does **not** yet provide per-request retry budgets, caching, rate limiting, TLS termination, Prometheus metrics, or statistically comparable load-test results. The underlying C++ proxy has more functionality. The Python unit tests pass locally; Rust build and smoke results should be determined from GitHub Actions.

## Next steps

1. Add bounded retry-on-connect for safe idempotent methods.
2. Add per-origin request counters and latency histogram.
3. Implement load test matching C++ workload, response size, concurrency and compiler settings.
4. Add a documented performance comparison and technical tradeoffs.

## References

- [Pingora repository](https://github.com/cloudflare/pingora)
- [Official Pingora quick start](https://github.com/cloudflare/pingora/blob/main/docs/quick_start.md)

## Retry and observability experiment (v0.3)

- **Connection retries:** For `GET` and `HEAD` only, upstream connection failures may be retried once; the request context tracks up to two backend selections and skips the failed address on a retry. `POST` and other methods are not explicitly retried.
- **Metrics:** Loopback-only `http://127.0.0.1:9188/metrics` exposes Prometheus-style request counts, terminal proxy errors, eligible connection retries and approximate request latency buckets (5/25/100/500 ms).
- **Failover exercise:** `python3 scripts/ci_failover.py` starts two synthetic origins, verifies distribution, stops one, checks convergence and restores it. Requires the proxy already running.
- **Scope:** Passive failure detection, full request retries, HTTP-status-based retries, auth, TLS, public access and production hardening are not included. An origin can fail during the health-check convergence window.
- **Benchmarks:** `scripts/benchmark.py` measures *serial* request latency only. Do not compare its output to concurrent C++ proxy throughput figures. Same-hardware concurrent load testing remains future work.

