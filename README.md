# edge-proxy

A small HTTP/1.1 **reverse proxy** in C++17, modelled on what a CDN edge node does:
it sits in front of a pool of origin servers and handles load balancing, health
checking, rate limiting, caching, and observability. Single-threaded, epoll-based,
about 2,500 lines of source (plus about 1,300 lines of tests), no dependencies beyond the standard library and POSIX.

```
                         ┌──────────────────── edge-proxy ─────────────────────┐
  clients ──HTTP/1.1──►  │ rate limit → cache ─hit─► respond                   │
                         │                 │miss                               │
                         │        coalesce identical misses (one fetch)        │
                         │                 ▼                                   │
                         │   balancer (round-robin | consistent hash)          │  ──► origin A
                         │     ▲ health state (active probes + passive)        │  ──► origin B
                         │     └─ retry on another backend if one fails        │  ──► origin C
                         └─────────────────────────────────────────────────────┘
                            :9100/metrics  (Prometheus)
```

## Rust/Pingora companion — EdgeGuard

This repository also contains an independently developed [Rust/Pingora proxy experiment](pingora-rust/) designed to explore health-aware routing, bounded connection retries and observability alongside the original C++ implementation.

- [Project architecture](pingora-rust/docs/ARCHITECTURE.md) · [Engineering case study](pingora-rust/docs/CASE_STUDY.md)
- [Verified benchmark and failure-injection report](pingora-rust/benchmarks/PHASE4_AND_PHASE5.md) · [CPU/RSS and repeated reliability observations](pingora-rust/benchmarks/PHASE6.md)
- [Upstream Pingora contribution plan](pingora-rust/docs/UPSTREAM_CONTRIBUTION_PLAN.md)

**These are separate implementations, not a production CDN.** Under the three-trial shared-runner comparison, median throughput was 5,103.91 requests/s for C++ and 5,076.47 for Rust; the difference is not conclusive. The source and methodology are linked above.

## Features

| Area | What it does |
|---|---|
| **Concurrency** | One epoll event loop (level-triggered), non-blocking sockets, no thread per connection. Partial reads/writes and `EAGAIN` handled everywhere. |
| **Load balancing** | Round-robin, or consistent hashing (128 virtual nodes/backend) on `host+path` so each URL lands on one backend and caches stay warm. When a backend dies, only *its* keys move (tested). |
| **Health checking** | *Active*: a probe thread requests `/health` with hard deadlines, using rise/fall thresholds so one blip doesn't flap a backend. *Passive*: 3 consecutive proxy failures eject a backend immediately. If everything is down, the balancer fails open instead of refusing all traffic. |
| **Failover** | `GET`/`HEAD` are idempotent, so a failed attempt is retried on a *different* backend before anything has been sent to the client. Clients see no errors when one origin crashes (measured, below). |
| **Caching** | Byte-bounded LRU with TTL (`Cache-Control: max-age`/`s-maxage` honoured; `no-store`, `no-cache`, `private`, `Set-Cookie`, `Vary` are never cached). Requests carrying `Authorization`, `Cookie`, `Range` or conditional headers bypass the cache. |
| **Request coalescing** | N simultaneous misses for one URL produce **one** origin request; the other N-1 wait and share the result (stops thundering herds). If the response turns out to be uncacheable, waiters are *not* handed someone else's copy; they are re-dispatched individually. |
| **Rate limiting** | Token bucket per client IP, `429` + `Retry-After`. Memory-bounded. |
| **Security hardening** | Rejects request-smuggling shapes (`Content-Length` + `Transfer-Encoding`, duplicate/conflicting `Content-Length`, whitespace before `:`, obsolete line folding, bare LF/NUL). Header size/count limits (`431`), slow-header (slowloris) deadline that trickled bytes cannot extend, connection limit, hop-by-hop header stripping, `X-Forwarded-For` appended from the real peer address. |
| **Backpressure & timeouts** | Slow readers pause the connection (`EPOLLOUT`); separate header, idle, write, connect and upstream timeouts; bounded response buffering. |
| **Observability** | Prometheus metrics on a separate loopback-only admin port: request/response counts, cache hit/miss/coalesced, rate-limited, upstream errors/retries/timeouts, per-backend health, active connections, latency histogram. |

## Build and run

Requires a C++17 compiler, CMake ≥ 3.16, and GoogleTest (`libgtest-dev`) for the tests.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # 72 tests

# demo: two toy origins and the proxy
build/fake-backend --port 9001 --id a &
build/fake-backend --port 9002 --id b &
build/edge-proxy --backend 127.0.0.1:9001 --backend 127.0.0.1:9002 &

curl -i http://127.0.0.1:8080/hello      # X-Cache: MISS
curl -i http://127.0.0.1:8080/hello      # X-Cache: HIT
curl -s http://127.0.0.1:9100/metrics | grep proxy_cache
```

Sanitizer builds: `-DSANITIZE=address` (ASan + UBSan) or `-DSANITIZE=thread`.
Run `edge-proxy --help` for all options (`--policy hash`, `--rate 100 --burst 200`, `--no-cache`, timeouts, …).

## Testing

72 tests: 42 unit and 30 end-to-end. The end-to-end tests start a real proxy and real
origin servers on ephemeral ports and drive them over sockets. They cover forwarding,
keep-alive and pipelining, chunked/close-delimited origins, caching and expiry,
**100 concurrent clients collapsing into one origin request**, round-robin and
consistent-hash routing, **killing an origin mid-traffic**, active health eject/restore,
rate limiting, smuggling payloads, slowloris, connection limits, upstream timeouts, and
clients that vanish mid-request.

The suite passes cleanly (10 consecutive runs) in a normal build and under
**AddressSanitizer + UBSan** and **ThreadSanitizer**. A soak run of the ASan build under
`wrk`, with an origin killed partway through, reported no errors and left no leaked
file descriptors.

## Benchmarks

`bench/run_bench.sh` reproduces everything below (needs `wrk`, `python3`, `curl`).

**Read these numbers with care.** They were measured on a shared 1-vCPU Linux sandbox
where `wrk`, the proxy and the three origins all compete for the same core, and the origins
are a toy thread-per-connection server. They show relative behaviour, not absolute capacity,
and are not comparable to production hardware or to other proxies. Re-run on your own
machine before quoting any of them.

| Scenario (8 s, 64 connections) | Requests/sec | p50 | p99 |
|---|---:|---:|---:|
| Direct to one toy origin (no proxy) | 123,575 | 0.26 ms | 0.78 ms |
| Proxy, **cache hit** path | 144,400 | 0.37 ms | 1.19 ms |
| Proxy, cache off (every request proxied, 3 origins) | 13,884 | 4.45 ms | 7.56 ms |

Cache on versus off is about **10×** the throughput and **12×** lower median latency on
this setup. (The cached path beating the direct baseline says more about the toy origin
than about the proxy.) With the cache off, each proxied request opens a fresh upstream
connection, which is the main cost; see "Limitations".

**Failover** (cache off, one of three origins `kill -9`'d 3 s into a 10 s load run):
134,514 requests, **0 errors returned to clients**; the proxy recorded 20 failed upstream
attempts, all retried on another backend, and ejected the dead backend automatically.

**Coalescing** (1,000 simultaneous requests for one uncached URL, origin delay 300 ms):
the origin received **1** request. All 1,000 clients got `200`; 1 was a `MISS` and 999
were `COALESCED`; everyone finished in 0.42 s.

## Limitations (deliberate scope cuts)

Documented honestly in [`docs/design.md`](docs/design.md), including what I would build next:

- `GET` and `HEAD` only; request bodies are rejected. No TLS, HTTP/2, WebSockets, or IPv6.
- Responses are buffered in full (bounded to 16 MiB) rather than streamed.
- A new upstream connection per request (no upstream keep-alive pool).
- `Vary` is not implemented: any response with `Vary` is simply not cached.
- One event-loop thread. Scaling across cores would use one loop per core with `SO_REUSEPORT`.
- The rate limiter keys on the TCP peer address; clients behind one NAT share a bucket.

## Layout

```
src/http.*            strict request parser, incremental response parser, header handling
src/event_loop.*      epoll reactor with generation-tagged registrations and deferred destruction
src/proxy.*           connection state machines, fetches, coalescing, caching policy, timeouts
src/balancer.*        round-robin and consistent-hash selection, health state
src/health_checker.*  active probe thread
src/cache.*           LRU+TTL cache          src/rate_limiter.h   token bucket
src/metrics.*         Prometheus rendering   src/main.cpp         CLI
testutil/             fake origin used by tests and benchmarks
tests/                unit and end-to-end tests
bench/                wrk scenarios, coalescing demo, recorded results
docs/design.md        design decisions, trade-offs, and interview notes
```
