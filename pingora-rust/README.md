# EdgeGuard — Rust + Cloudflare Pingora

Independent Rust networking prototype and engineering comparison with the [C++17 Linux edge proxy](../README.md). Built using the open-source [Pingora](https://github.com/cloudflare/pingora) framework. **Not affiliated with or endorsed by Cloudflare.**

## Navigation

- [Architecture diagram and boundaries](docs/ARCHITECTURE.md)
- [Engineering case study](docs/CASE_STUDY.md)
- [Reproducible benchmark protocol](benchmarks/README.md)
- [Verified throughput/failover measurements](benchmarks/PHASE4_AND_PHASE5.md)
- [Verified process-resource and repeated outage results](benchmarks/PHASE6.md)
- [Upstream contribution plan (not submitted)](docs/UPSTREAM_CONTRIBUTION_PLAN.md)

## Features already implemented

- HTTP proxying through Pingora and round-robin origin routing
- Background TCP health checks and unavailable-origin handling
- At most one additional *connection* attempt for eligible GET/HEAD requests, with a distinct-backend retry selection
- Loopback-only Prometheus-style metrics at `127.0.0.1:9188/metrics` (request/error/retry counters and rough duration buckets)
- Synthetic origin fixtures and automated origin-failure/restoration checks
- Three-run paired throughput benchmark, failure-injection results, and Linux process CPU/RSS sampling
- CI verification using Rust/Python unit tests, C++ builds and integration tests

## Run locally (Linux)

Requirements: stable Rust/Cargo, `pkg-config`, OpenSSL development headers and Python 3.

In three terminals from this directory:

```bash
python3 scripts/origin.py --port 9001 --name origin-a
python3 scripts/origin.py --port 9002 --name origin-b
cargo run --release
```

Then in a fourth:

```bash
python3 scripts/smoke_test.py --requests 30
curl -s http://127.0.0.1:9188/metrics
python3 -m unittest discover -s tests -v
```

Optional configuration:

```bash
EDGEGUARD_LISTEN=127.0.0.1:6188 \
EDGEGUARD_METRICS_LISTEN=127.0.0.1:9188 \
EDGEGUARD_BACKENDS=127.0.0.1:9001,127.0.0.1:9002 \
cargo run --release
```

Test a normal health-check transition by stopping origin A, waiting several seconds, and running `python3 scripts/failover_test.py --expected-origin origin-b`. Restart origin A and allow health checks to recover.

## Run comparative experiments

From the repository root, build the C++ proxy using `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`. From this directory run:

```bash
cargo build --release
python3 scripts/compare_proxies.py --duration 8 --connections 32 --threads 2 --repeat 3
python3 scripts/failure_injection.py --duration 11 --concurrency 4 --repeat 3
```

For the repeatability and limitations of these commands, see [the benchmark protocol](benchmarks/README.md). The CI workflow automates build, test, throughput and origin-failure checks.

## Verified observations — shared CI, not a ranking

| Measure | C++17 | Rust/Pingora |
|---|---:|---:|
| Median throughput, three runs | 5,103.91 req/s | 5,076.47 req/s |
| Outage-phase errors, three low-concurrency trials | 0 | 1 |
| Median first observed return to restored origin | 1.7076 s | 0.7250 s |
| Median peak process RSS | 4.03 MiB | 15.76 MiB |

The high tail latency, synthetic origins and shared runner preclude claims about production performance or language superiority. Link to the original CI artifacts and method when quoting these results.

## Limitations

This Rust prototype does **not** implement caching, token-bucket rate limiting, production-grade TLS, full HTTP hardening or feature parity with the C++ proxy. Prometheus-style metrics are approximate, process samples exclude child origins, and origin failover can still produce request errors. Neither implementation should be used as a publicly exposed production service without extensive security review.

## References

- [Pingora quick start](https://github.com/cloudflare/pingora/blob/main/docs/quick_start.md)
- [Pingora contribution guidelines](https://github.com/cloudflare/pingora/blob/main/.github/CONTRIBUTING.md)
