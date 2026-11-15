# Same-host C++ vs Rust/Pingora benchmark protocol

This folder provides an **experiment**, not a performance claim. No figures are published until the commands complete on the same host, with raw logs retained.

## Prerequisites (Linux)

- `g++`, `cmake`, Rust/Cargo, `pkg-config`, `libssl-dev`, `python3`, `wrk`, and `libgtest-dev`
- An otherwise idle machine. Keep CPU power governor, thermal conditions and Rust compiler version documented.

```bash
sudo apt-get update
sudo apt-get install -y g++ cmake libgtest-dev pkg-config libssl-dev wrk python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd pingora-rust && cargo build --release && cd ..
python3 pingora-rust/scripts/compare_proxies.py --duration 8 --connections 32 --threads 2 --repeat 3
```

The script runs the same two Python HTTP origins for both proxies. It uses round-robin GET forwarding with C++ caching disabled, equal connection concurrency and identical request URL. The Rust server's default threading and the single-threaded C++ reactor are **not equivalent configurations**. The origin and wrk generator share CPU/memory with the proxies and can become the bottleneck.

## Percentiles

Standard `wrk --latency` reports p50, p75, p90, and p99. It does **not** report p95; we neither invent nor interpolate p95. Raw output remains the source of truth.

## Measurements

Each iteration includes a 2-second warmup followed by a timed load phase. The order alternates C++/Rust, then Rust/C++, then C++/Rust to reduce ordering bias. Artifacts contain raw `wrk` output, the corresponding proxy metrics, and a machine-readable `results.json`. Record the exact Git commit and host CPU/memory/kernel separately in any publication.

Interpret `wrk` p50/p75/p90/p99 as request-latency observations **from wrk**, not proxy-internal histograms. Throughput and latency are aggregate and affected by upstream connection reuse. Keep all errors, including non-2xx responses and socket timeouts, visible.

**Do not** compare numbers from this run with the original C++ historical benchmark: different origins, number of backends, client conditions and hardware may invalidate conclusions. Running on shared GitHub Actions runners provides basic regression signals, not portable performance rankings.

## Next experiment

Measure failover under sustained load while terminating one origin; record seconds to stable healthy-only traffic, errors during transition and backend recovery. This will be a separate failure-injection report, not folded into clean throughput numbers.

## Repeated-run summary

The CI workflow now runs **three trials per proxy**, with a 6-second measured phase per trial, 32 connections and two wrk threads. The order alternates between implementations. The helper `python3 scripts/summarize_comparison.py benchmarks/results/ci/results.json` writes `summary.json` and `summary.md` with the median and [minimum, maximum] observed within the run; it also reports response and socket errors. No automatic winner is declared.

These are shared-runner smoke measurements, not statistically independent controlled experiments. Three short trials and the thread-per-connection Python origin limit how much we can infer; validation on a dedicated Linux host is required before using comparative performance claims in job applications.
