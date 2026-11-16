# Phase 6 — Process observability and repeated failure injection

The Phase 6 workflow repeats the low-concurrency origin failure exercise **three times for each proxy**, alternating execution order. It uses the identical synthetic origins, 11-second trials, 4 client threads and approximately 2s/6s outage/restore schedule.

Each trial stores raw request-level outcomes, proxy metrics, captured startup logs and process-specific resource summaries. Linux `/proc` CPU accounting and RSS readings are sampled at roughly 200ms intervals. CPU is reported in units of **one logical core** (100% = one fully utilized core); RSS is resident memory for the proxy PID, not the entire workload.

The report summarizes median observed origin-return times, ranges, error counts and peak RSS. CI results are subject to shared-runner noise and short window effects; they are not proof of production availability. The architecture and origin bottlenecks differ between C++ and Rust, and CPU/RSS comparisons do not establish language superiority.

CI will generate `phase6_summary.md` and `phase6_summary.json` in the failure-injection artifact. **Do not add numbers to this document until a successful new run verifies them.**

Previous verified Phase 4/5 measurements: [PHASE4_AND_PHASE5.md](PHASE4_AND_PHASE5.md).

## Verified three-trial results (GitHub Actions)

Workflow [Pingora Rust prototype #14](https://github.com/olabs123-hue/Proxy/actions/runs/43777732244) for commit `d5bb4c0` completed successfully. The [raw failure-injection artifact](https://github.com/olabs123-hue/Proxy/actions/runs/43777732244/artifacts/14187143047) contains per-trial events, proxy metrics, startup logs and process-resource summaries.

| Recorded metric | C++17 | Rust/Pingora |
|---|---:|---:|
| Trials | 3 | 3 |
| Outage-phase requests (total) | 3,148 | 3,127 |
| Outage-phase errors (total) | 0 | 1 |
| Median time until first observed response from restored origin | 1.7076 s | 0.7250 s |
| Observed recovery range | 1.6935–1.7163 s | 0.7204–0.7263 s |
| Median of trial mean process CPU (% of one core) | 2.85% | 4.35% |
| Median of trial peak process RSS | 4.03 MiB | 15.76 MiB |

**Interpretation:** C++ consumed less sampled process memory and CPU in this synthetic run, while Rust/Pingora routed requests to the restored origin sooner. The Rust implementation recorded **one** request error during the outage across three trials. These are observations, not evidence of an intrinsic language/framework advantage. CPU percentages refer to one logical core; memory excludes child origins and load generation. First observed response does not directly measure internal health-check convergence.

**Next engineering work:** Add separate high-load failure injection, more independent repetitions on controlled hardware, and a measurement of the failure-transition window. Do not claim zero-error failover or production SLAs based on this experiment.
