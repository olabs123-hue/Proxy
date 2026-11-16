# EdgeGuard — engineering case study

**Project:** Independent comparison of a C++17 Linux reverse proxy and a Rust implementation using Cloudflare's open-source Pingora. **Not affiliated with or endorsed by Cloudflare.**

## Problem

A proxy at the network edge must route traffic to healthy origin servers, tolerate origin failures and provide observability. My original C++17 proxy implemented an epoll-based HTTP/1.1 stack with caching, load balancing, retries, rate limiting and metrics. I used a second Rust/Pingora experiment to compare architectures on shared local test workloads without implying identical feature coverage.

## What I built

- A Rust/Pingora reverse proxy with round-robin selection, TCP health checks, bounded GET/HEAD connection retries and loopback-only Prometheus-style metrics.
- Python integration tooling that launches local origins, kills/restores an origin, collects request-level outcomes, and measures process-only CPU/RSS.
- Three-run paired `wrk` benchmarks with raw output and median/range summaries, plus repeatable failure-injection CI tests.
- Linux/C++ and Rust CI workflows. The C++ source remains separate and unchanged by the Rust experiment.

## Verified observations and evidence

### Throughput experiment — [CI run](https://github.com/olabs123-hue/Proxy/actions/runs/43770451231)

Three sequential six-second trials per proxy on a shared GitHub Actions runner, 32 client connections, two Python origins, C++ caching disabled:

| Median measure | C++17 | Rust/Pingora |
|---|---:|---:|
| Throughput | 5,103.91 requests/s | 5,076.47 requests/s |
| p50 latency | 2.37 ms | 2.34 ms |
| p99 latency | 1,130 ms | 1,110 ms |
| Socket errors across trials | 17 | 15 |

The throughput difference was roughly 0.5%, with overlapping ranges and unusually high tail latency. These data do **not** establish an intrinsic speed advantage for either implementation.

### Origin failure and resource experiment — [CI run](https://github.com/olabs123-hue/Proxy/actions/runs/43777732244)

Three low-concurrency failure-injection trials per proxy; four client threads, origin A terminated around two seconds and restarted around six seconds:

| Observed measure | C++17 | Rust/Pingora |
|---|---:|---:|
| Requests during origin outage | 3,148 | 3,127 |
| Errors during outage | 0 | 1 |
| Median first observed successful routing to restored origin | 1.7076 s | 0.7250 s |
| Median of trial-average process CPU | 2.85% of one core | 4.35% of one core |
| Median trial-peak resident memory | 4.03 MiB | 15.76 MiB |

These are **observed test outcomes**, not proof of zero downtime, production availability or faster internal health detection. The low concurrency and shared machine do not support universal comparisons.

## Key technical tradeoffs

- **Build vs framework:** The C++ proxy owns more HTTP parsing/timeout machinery; Pingora provides reusable proxy infrastructure and a different async execution model.
- **Reliability:** Active health checks can take time to converge. Retrying eligible connection failures helps but does not guarantee zero errors in every transition.
- **Observability:** Percentiles require consistent workload and correct interpretation. Standard `wrk --latency` supplies p50/p75/p90/p99; p95 must not be fabricated.
- **Methodology:** Preserve raw logs, alternate execution order, disclose shared-origin bottlenecks, and report errors alongside throughput.
- **Limits:** No TLS termination, public deployment, complete functional parity, or production SLOs are claimed for the Rust version.

## Source and reproduction

- [C++ implementation](../../README.md)
- [Rust implementation](../README.md)
- [Architecture](ARCHITECTURE.md)
- [Benchmark procedure](../benchmarks/README.md)
- [Verified Phases 4 and 5](../benchmarks/PHASE4_AND_PHASE5.md)
- [Verified Phase 6](../benchmarks/PHASE6.md)
- [GitHub Actions workflow](../../.github/workflows/pingora-rust.yml)

## Next learning milestones

Study Pingora's contribution guidelines, identify a specific documentation/test gap, submit a small upstream contribution only after checking its current relevance, and validate performance on dedicated hardware before making broader claims.
