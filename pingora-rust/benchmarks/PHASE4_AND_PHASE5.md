# Phase 4 — Three-run C++ vs Pingora comparison

**Evidence:** GitHub Actions workflow #11, commit `12c5012`, run [43770451231](https://github.com/olabs123-hue/Proxy/actions/runs/43770451231). Source is the run's `edgeguard-comparison-raw` artifact and job logs. This report quotes recorded measurements; no new benchmark runs were invented.

## Workload

- GitHub-hosted shared Ubuntu runner, 2 Python `ThreadingHTTPServer` origins
- Three six-second measurement windows per implementation; two-second warmup each
- 32 connections, 2 wrk threads, HTTP/1.1 GET, round-robin, C++ response caching disabled
- C++ single-threaded epoll vs Pingora's multithreaded architecture; workloads match but internals do not

| Metric (median of three trials) | C++17 | Rust/Pingora |
|---|---:|---:|
| Throughput (requests/sec) | 5,103.91 | 5,076.47 |
| p50 latency (ms) | 2.37 | 2.34 |
| p75 latency (ms) | 351.46 | 368.10 |
| p90 latency (ms) | 789.29 | 788.63 |
| p99 latency (ms) | 1,130 | 1,110 |
| Socket errors (total over three trials) | 17 | 15 |
| Non-2xx/3xx responses (total) | 0 | 0 |

**Range of observed throughput:** C++ 5,008.53–5,181.27 req/s; Rust 5,032.76–5,093.64 req/s.

**Interpretation:** ~0.5% difference in median throughput, with overlapping ranges. The high tail latencies and nonzero socket errors indicate the shared origin/runner/load generator can be a bottleneck. The data do not establish that C++ or Rust is faster. The historical C++ README benchmark is *not* comparable.

**Reproduction:** See [benchmark protocol](README.md). Run `python3 scripts/compare_proxies.py --duration 8 --connections 32 --threads 2 --repeat 3` from `pingora-rust/` after building both release executables from repository root.

## Phase 5 — Failure-injection measurement

The script `scripts/failure_injection.py` runs each proxy with two synthetic origins, records low-concurrency requests, terminates origin A at approximately 2 seconds, restarts it at approximately 6 seconds, and continues to observe until 11 seconds. It records phase-specific HTTP success/error counts and time until restored origin A is first observed receiving a successful request.

The timing value is **observed return to rotation**, not exact internal health detection or zero-downtime failover. Requests that straddle transitions can produce errors. Results are written to `benchmarks/results/failure-injection/` and uploaded as a separate CI artifact.

### Verified Phase 5 run

GitHub Actions [run #12](https://github.com/olabs123-hue/Proxy/actions/runs/43772973756) completed successfully, and its [failure-injection artifact](https://github.com/olabs123-hue/Proxy/actions/runs/43772973756/artifacts/14185422752) contains the request-level observations, proxy metrics and `failure_report.json`.

| Recorded observation | C++17 | Rust/Pingora |
|---|---:|---:|
| Baseline successful requests | 512 / 512 | 510 / 510 |
| Requests during origin-A outage | 1,052 | 1,052 |
| Successful requests during outage | 1,052 | 1,052 |
| Errors during outage | 0 | 0 |
| Recovery-phase successful requests | 1,236 / 1,236 | 1,234 / 1,234 |
| First origin-A response observed after restart | 1.7026 s | 0.7262 s |
| Total errors (baseline + outage + recovery) | 0 | 0 |

**Protocol:** The tester launched two Python origins and one proxy at a time, issuing concurrent requests from four client threads. It terminated origin A at approximately 2 seconds, restarted A at approximately 6 seconds, and sampled traffic for roughly 11 seconds in each implementation.

**Interpret carefully:** "Time to first observed response" is measured from the observed successful origin restart, not from the instant internal health checks recognized recovery. The script starts/stops origins in the same host; the sample is neither a production-style sustained-load test nor a demonstration of error-free operation under every possible outage timing. Zero observed errors in one short test is not a zero-downtime guarantee. Recovery figures are not sufficient to conclude Rust/Pingora is intrinsically faster.

**Next:** Repeat across runs with controlled hardware, sample process-level CPU and resident memory, test different concurrency levels, and intentionally kill an origin under higher load to estimate transition errors and detection delays.

