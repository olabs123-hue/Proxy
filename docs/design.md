# Design notes

This document explains the decisions behind edge-proxy, what each one costs, and what
I would do differently at larger scale. It doubles as preparation for the questions
a reviewer is likely to ask.

## 1. Event loop: epoll, level-triggered, one thread

**Why epoll instead of a thread per connection?** A proxy spends nearly all its time
waiting on sockets. Threads cost memory (stack) and context switches per connection;
an event loop holds a few hundred bytes of state per connection and scales to thousands
of idle keep-alive clients.

**Level- vs edge-triggered.** I used level-triggered. Edge-triggered saves some syscalls,
but you must drain each socket until `EAGAIN` or you stall it forever, and a fairness
bug lets one busy connection starve the others. Level-triggered is harder to get wrong,
and `recv` is capped at one 16 KiB read per event so no connection monopolises the loop.
Moving hot paths to edge-triggered is a later optimisation, to be justified by a profile.

**A bug class the loop is built to prevent.** In one `epoll_wait` batch, a handler can close
fd 12 and a later handler can `accept()` a new connection that reuses number 12; the stale
event for the old socket then arrives at the new owner. Each registration therefore carries
a generation number in the event data, and mismatches are dropped. Objects are also never
destroyed from inside their own handler: `close_client()` marks the connection closed and
schedules destruction with `defer()`, which runs after the batch.

**Scaling across cores.** Single-threaded by choice: no locks on the hot path, and the cache,
rate limiter and metrics are plain data structures. The scale-out design is one loop per
core, each with its own `SO_REUSEPORT` listener (the kernel spreads connections), sharing
nothing. Cache and rate-limit state would then be per-core, so the cache hit rate drops
unless requests are steered by URL. I'd put consistent hashing in front, or accept
duplication across cores, and measure.

## 2. Connection state machine

Client side: `ReadingRequest → WaitingUpstream → Writing → ReadingRequest`.
While waiting or writing, the connection's read interest is switched off, which gives
natural backpressure (a client cannot pipeline unbounded data at us) and means at most one
buffered read of pipelined bytes. Those bytes are processed after the response completes.
`process_requests` has a re-entrancy guard so a run of cache hits on one connection is a loop,
not recursion.

Slow clients: if `sendmsg` returns `EAGAIN`, the connection switches to `EPOLLOUT` and resumes
when the socket drains, with a write deadline so a stalled reader cannot hold memory forever.

## 3. Strict HTTP parsing and request smuggling

Smuggling works when two HTTP parsers (proxy and origin) disagree about where a request ends.
The cheapest defence is for the proxy to refuse ambiguity:

- `Content-Length` together with `Transfer-Encoding` is rejected (`400`). This is the classic CL.TE/TE.CL setup.
- Duplicate `Content-Length` headers with different values are rejected.
- `Header : value` (whitespace before the colon) and obsolete line folding are rejected.
- Bare `LF`, `CR` or `NUL` inside a header block are rejected.
- Request bodies and `Transfer-Encoding` on requests are unsupported by design, so there is no body-framing logic to get wrong.
- After any parse error the connection is closed, never reused.

Because the proxy re-serialises the request to the origin (rather than forwarding raw bytes),
only headers it understood and validated are passed on, and hop-by-hop headers (including any
named in `Connection:`) are stripped.

## 4. Caching

- **Key**: `lowercase(Host) + request-target` (query string included).
- **Eligibility** is conservative. Never cache: non-200s, responses with `Set-Cookie`, `Vary`,
  `no-store`/`no-cache`/`private`. Never serve from or fill the cache for requests with
  `Authorization`, `Cookie`, `Range` or conditional headers, or `Cache-Control: no-cache`.
  A wrong cache hit on a personalised page is a data leak, which is worse than a miss.
- **TTL** comes from `s-maxage`, then `max-age`, else a configured default.
- **Eviction**: LRU bounded by bytes (not entry count), with an object-size cap so one large file cannot flush everything. Entries are `shared_ptr`, so a response being written to a slow client stays alive after eviction.

## 5. Request coalescing (the thundering-herd problem)

When a popular object expires, hundreds of requests arrive before the first refill completes.
Without protection, all of them go to the origin, which is exactly when it is already slow.

On a miss, the first request becomes the *leader* and registers `inflight[key]`. Later misses
for the same key attach to the leader's fetch as *waiters*. When the response arrives it is
cached and handed to everyone. Tested: 100 concurrent clients produce 1 origin request, and a
1,000-client demo does the same.

**The subtle part: uncacheable responses.** If the response is `no-store` or personalised,
sharing the leader's copy with the waiters would hand one user's data to another. So the leader
gets its own response, the key is marked "pass" for a few seconds (the idea behind Varnish's
hit-for-pass), and the waiters are re-dispatched as independent fetches. Without the pass marker
those re-dispatched requests would coalesce again and serialise, one origin round trip after another.

**Failures** are shared with all waiters (`502`/`504`) so a broken origin is not hit once per
waiter. **Cost**: if a leader's client disconnects, the fetch still completes, because the cache fill
benefits others. Fetches are bounded by the upstream timeout.

## 6. Load balancing and health

**Consistent hashing** uses a ring with 128 virtual nodes per backend (to even out the load) and
hashes `host+path`, so each URL maps to one backend and that backend's cache stays warm. When a
backend is unhealthy the lookup walks clockwise to the next healthy node, so *only the failed
backend's keys move*, and when it recovers they move back. There is a unit test for exactly this.

**Health is checked two ways**, because each covers the other's blind spot:

- *Active* probes catch a backend that is down but receiving no traffic, and detect recovery.
  They run on a dedicated thread with blocking sockets and hard `poll()` deadlines, so a hung
  backend can never stall the event loop. The thread touches only atomics.
  `rise`/`fall` thresholds (2/2 by default) stop one lost probe from flapping a backend.
- *Passive* checking reacts within milliseconds: 3 consecutive failed proxied requests eject a
  backend without waiting for the next probe.

**Fail-open**: if every backend is marked unhealthy the balancer still picks one. Refusing all
traffic because the health signal might be wrong is a worse failure than trying.

**Retries.** `GET`/`HEAD` are idempotent and responses are fully buffered, so nothing has reached the
client when an attempt fails. Retrying on a *different* backend (the failed ones are excluded) is safe.
Only 2 attempts by default: more would multiply load on a struggling pool (retry amplification).

## 7. Timeouts and resource limits

| Limit | Default | Protects against |
|---|---|---|
| header timeout | 10 s | slowloris. The deadline starts at the first byte and is **not** extended by later bytes. |
| idle timeout | 30 s | abandoned keep-alive connections |
| write timeout | 30 s | clients that stop reading |
| connect / upstream timeout | 2 s / 10 s | hung origins |
| max connections | 10,000 | fd exhaustion; at `EMFILE` the proxy sheds the pending connection using a reserved spare fd, so the listener does not spin |
| header size / count | 16 KiB / 100 | memory abuse (`431`) |
| response size | 16 MiB | a rogue origin exhausting memory (`502`) |

## 8. Observability

Metrics live on a separate loopback-only admin listener, so they are not reachable through the
public port. They are Prometheus text format: counters, per-backend health gauges, and a latency
histogram (buckets from 0.5 ms to 5 s) so p50/p99 come from `histogram_quantile` rather than being
averaged away.

## 9. Known limitations and what I would build next

1. **Upstream connection pooling.** Each proxied request opens a new TCP connection and sends
   `Connection: close`. This is the dominant cost in the cache-off benchmark. A keep-alive pool with
   health-aware reuse (and handling the stale-connection race) is the highest-value next step.
2. **Streaming.** Responses are buffered, so a 1 GiB download would be rejected (`502`) and time to
   first byte equals time to last byte. Streaming needs per-connection flow control between upstream and downstream.
3. **`Vary` support**, conditional revalidation (`If-None-Match` to refresh instead of refetch), and
   stale-while-revalidate/serve-stale-on-error.
4. **TLS and HTTP/2** (OpenSSL/BoringSSL with non-blocking handshakes; ALPN).
5. **Multi-core** as in section 1, with `SO_REUSEPORT`.
6. **Rate limiting by more than peer IP** (clients behind one NAT share a bucket; `X-Forwarded-For` must only be trusted from known proxies).
7. **IPv6**, and DNS re-resolution of backends (they are resolved once at startup).
8. **Fuzzing** the parsers with libFuzzer. The incremental response parser is tested against byte-by-byte feeds, but a fuzzer would find what hand-written cases miss.

## 10. Questions to be ready for

- *Why epoll over threads? Why level-triggered?* Sections 1.
- *What happens when a backend dies mid-response?* The attempt fails (EOF/reset), the backend is marked, and since nothing was sent to the client the request is retried on another backend. Test: `RequestsFailOverWhenABackendDiesMidTraffic`.
- *How do you avoid the thundering herd?* Section 5.
- *How would you scale to many cores?* Section 1, and the cache-locality trade-off.
- *What is request smuggling and how does this proxy resist it?* Section 3.
- *What would you change first?* Upstream keep-alive pooling (section 9, item 1), because it is the measured bottleneck.
- *How do you know it is correct under concurrency?* ThreadSanitizer, AddressSanitizer, UBSan, plus a repeated end-to-end suite and a soak run under load with a backend killed.
