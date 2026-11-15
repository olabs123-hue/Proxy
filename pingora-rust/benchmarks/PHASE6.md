# Phase 6 — Process observability and repeated failure injection

The Phase 6 workflow repeats the low-concurrency origin failure exercise **three times for each proxy**, alternating execution order. It uses the identical synthetic origins, 11-second trials, 4 client threads and approximately 2s/6s outage/restore schedule.

Each trial stores raw request-level outcomes, proxy metrics, captured startup logs and process-specific resource summaries. Linux `/proc` CPU accounting and RSS readings are sampled at roughly 200ms intervals. CPU is reported in units of **one logical core** (100% = one fully utilized core); RSS is resident memory for the proxy PID, not the entire workload.

The report summarizes median observed origin-return times, ranges, error counts and peak RSS. CI results are subject to shared-runner noise and short window effects; they are not proof of production availability. The architecture and origin bottlenecks differ between C++ and Rust, and CPU/RSS comparisons do not establish language superiority.

CI will generate `phase6_summary.md` and `phase6_summary.json` in the failure-injection artifact. **Do not add numbers to this document until a successful new run verifies them.**

Previous verified Phase 4/5 measurements: [PHASE4_AND_PHASE5.md](PHASE4_AND_PHASE5.md).
