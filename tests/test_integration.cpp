// End-to-end tests: a real proxy (on ephemeral ports) in front of real fake
// origins, driven over real sockets.
#include <gtest/gtest.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <set>
#include <map>
#include <memory>
#include <thread>

#include "fake_backend.h"
#include "proxy.h"

using namespace edge;
using testutil::FakeBackend;

namespace {

struct HttpResult {
  int status = 0;
  std::map<std::string, std::string> headers;  // lower-cased names
  std::string body;
  bool ok = false;
};

int connect_to(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sa.sin_port = htons(port);
  if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
    close(fd);
    return -1;
  }
  timeval tv{5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

void send_all(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

// Reads one response from a (possibly kept-alive) connection. `buf` carries
// leftover bytes between calls so pipelined responses can be parsed in turn.
HttpResult read_response(int fd, std::string& buf, bool head_request = false) {
  HttpResult r;
  char tmp[8192];
  size_t end;
  while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n <= 0) return r;
    buf.append(tmp, static_cast<size_t>(n));
  }
  std::string head = buf.substr(0, end);
  buf.erase(0, end + 4);
  r.status = std::atoi(head.c_str() + 9);
  size_t pos = head.find("\r\n");
  while (pos != std::string::npos) {
    size_t next = head.find("\r\n", pos + 2);
    std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string v = line.substr(colon + 1);
      v.erase(0, v.find_first_not_of(' '));
      r.headers[to_lower(line.substr(0, colon))] = v;
    }
    pos = next;
  }
  size_t len = r.headers.count("content-length") ? std::stoul(r.headers["content-length"]) : 0;
  if (head_request) len = 0;
  while (buf.size() < len) {
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n <= 0) return r;
    buf.append(tmp, static_cast<size_t>(n));
  }
  r.body = buf.substr(0, len);
  buf.erase(0, len);
  r.ok = true;
  return r;
}

HttpResult http(uint16_t port, const std::string& method, const std::string& path, const std::string& extra = "") {
  int fd = connect_to(port);
  HttpResult r;
  if (fd < 0) return r;
  send_all(fd, method + " " + path + " HTTP/1.1\r\nHost: test.local\r\nConnection: close\r\n" + extra + "\r\n");
  std::string buf;
  r = read_response(fd, buf, method == "HEAD");
  close(fd);
  return r;
}
HttpResult get(uint16_t port, const std::string& path, const std::string& extra = "") {
  return http(port, "GET", path, extra);
}

// Sends raw bytes and returns everything received until the peer closes.
std::string raw_exchange(uint16_t port, const std::string& raw) {
  int fd = connect_to(port);
  if (fd < 0) return "";
  send_all(fd, raw);
  std::string out;
  char tmp[4096];
  for (;;) {
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n <= 0) break;
    out.append(tmp, static_cast<size_t>(n));
  }
  close(fd);
  return out;
}

double metric(const std::string& text, const std::string& name) {
  size_t pos = 0;
  while ((pos = text.find(name, pos)) != std::string::npos) {
    bool line_start = pos == 0 || text[pos - 1] == '\n';
    size_t after = pos + name.size();
    if (line_start && after < text.size() && text[after] == ' ') return std::atof(text.c_str() + after + 1);
    pos = after;
  }
  return -1;
}

struct Rig {
  std::vector<std::unique_ptr<FakeBackend>> backends;
  std::unique_ptr<Proxy> proxy;
  std::thread th;

  Rig(int n_backends, const std::function<void(Config&)>& tweak = nullptr, int backend_delay_ms = 0) {
    Config cfg;
    cfg.listen_port = 0;
    cfg.admin_port = 0;
    cfg.health.interval = 60;  // keep active probing out of the way unless a test wants it
    for (int i = 0; i < n_backends; ++i) {
      backends.push_back(std::make_unique<FakeBackend>(std::string(1, static_cast<char>('a' + i)), 0, backend_delay_ms));
      EXPECT_TRUE(backends.back()->start());
      cfg.backends.push_back(backends.back()->address());
    }
    if (tweak) tweak(cfg);
    proxy = std::make_unique<Proxy>(cfg);
    std::string err;
    EXPECT_TRUE(proxy->init(&err)) << err;
    th = std::thread([this] { proxy->run(); });
  }
  ~Rig() {
    proxy->stop();
    th.join();
  }
  uint16_t port() const { return proxy->port(); }
  std::string metrics() {
    int fd = connect_to(proxy->admin_port());
    send_all(fd, "GET /metrics HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    std::string buf;
    auto r = read_response(fd, buf);
    close(fd);
    return r.body;
  }
};

bool wait_for(const std::function<bool()>& cond, double seconds) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return cond();
}

}  // namespace

// ---------------------------------------------------------------------------
// Forwarding and framing
// ---------------------------------------------------------------------------

TEST(Integration, ForwardsRequestsToAnOrigin) {
  Rig rig(1);
  auto r = get(rig.port(), "/hello");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body, "backend:a path:/hello\n");
  EXPECT_EQ(r.headers["x-backend"], "a");
  EXPECT_EQ(rig.backends[0]->hits(), 1u);
}

TEST(Integration, KeepAliveServesManyRequestsOnOneConnection) {
  Rig rig(1, [](Config& c) { c.cache_enabled = false; });
  int fd = connect_to(rig.port());
  ASSERT_GE(fd, 0);
  std::string buf;
  for (int i = 0; i < 5; ++i) {
    send_all(fd, "GET /r" + std::to_string(i) + " HTTP/1.1\r\nHost: h\r\n\r\n");
    auto r = read_response(fd, buf);
    ASSERT_TRUE(r.ok) << i;
    EXPECT_EQ(r.body, "backend:a path:/r" + std::to_string(i) + "\n");
    EXPECT_EQ(r.headers["connection"], "keep-alive");
  }
  close(fd);
}

TEST(Integration, PipelinedRequestsAreAnsweredInOrder) {
  Rig rig(1, [](Config& c) { c.cache_enabled = false; });
  int fd = connect_to(rig.port());
  ASSERT_GE(fd, 0);
  std::string two = "GET /first HTTP/1.1\r\nHost: h\r\n\r\nGET /second HTTP/1.1\r\nHost: h\r\n\r\n";
  send_all(fd, two);
  std::string buf;
  auto a = read_response(fd, buf);
  auto b = read_response(fd, buf);
  EXPECT_EQ(a.body, "backend:a path:/first\n");
  EXPECT_EQ(b.body, "backend:a path:/second\n");
  close(fd);
}

TEST(Integration, HeadReturnsHeadersWithoutBody) {
  Rig rig(1);
  auto r = http(rig.port(), "HEAD", "/hello");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.status, 200);
  EXPECT_TRUE(r.body.empty());
  EXPECT_EQ(r.headers["x-cache"], "BYPASS");
}

TEST(Integration, ChunkedOriginResponsesAreDecoded) {
  Rig rig(1);
  auto r = get(rig.port(), "/chunked");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.body, "hello world");
  EXPECT_EQ(r.headers["content-length"], "11");
  EXPECT_EQ(r.headers.count("transfer-encoding"), 0u);
}

TEST(Integration, CloseDelimitedOriginResponsesWork) {
  Rig rig(1);
  auto r = get(rig.port(), "/close");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body, "backend:a path:/close\n");
}

TEST(Integration, UnsupportedMethodsGet405) {
  Rig rig(1);
  auto r = http(rig.port(), "POST", "/x");
  EXPECT_EQ(r.status, 405);
  EXPECT_EQ(r.headers["allow"], "GET, HEAD");
  EXPECT_EQ(rig.backends[0]->hits(), 0u);
}

// ---------------------------------------------------------------------------
// Security: malformed and smuggling-style requests never reach the origin
// ---------------------------------------------------------------------------

TEST(Integration, SmugglingAttemptsAreRejectedAndNeverForwarded) {
  Rig rig(1);
  const std::vector<std::pair<std::string, int>> cases = {
      {"GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
      {"GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello", 400},
      {"GET / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 501},
      {"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400},
      {"GET / HTTP/1.1\r\nHost: h\r\nContent-Length : 0\r\n\r\n", 400},
      {"GET / HTTP/1.1\r\n\r\n", 400},
      {"GET / HTTP/3.0\r\nHost: h\r\n\r\n", 505},
  };
  for (const auto& c : cases) {
    std::string resp = raw_exchange(rig.port(), c.first);
    ASSERT_GE(resp.size(), 12u) << c.first;
    EXPECT_EQ(std::atoi(resp.c_str() + 9), c.second) << c.first;
    EXPECT_NE(resp.find("Connection: close"), std::string::npos);  // connection is dropped after a framing error
  }
  EXPECT_EQ(rig.backends[0]->hits(), 0u);
  EXPECT_GE(metric(rig.metrics(), "proxy_bad_requests_total"), static_cast<double>(cases.size()));
}

TEST(Integration, OversizedHeadersGet431) {
  Rig rig(1);
  std::string resp = raw_exchange(rig.port(), "GET / HTTP/1.1\r\nHost: h\r\nX-Big: " + std::string(40000, 'a') + "\r\n\r\n");
  ASSERT_GE(resp.size(), 12u);
  EXPECT_EQ(std::atoi(resp.c_str() + 9), 431);
}

TEST(Integration, SlowHeaderClientsAreDisconnected) {
  Rig rig(1, [](Config& c) { c.header_timeout = 0.5; });
  int fd = connect_to(rig.port());
  ASSERT_GE(fd, 0);
  send_all(fd, "GET / HTT");  // never finishes the request line
  auto start = std::chrono::steady_clock::now();
  std::string got;
  char tmp[512];
  for (;;) {
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n <= 0) break;
    got.append(tmp, static_cast<size_t>(n));
  }
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  close(fd);
  EXPECT_LT(elapsed, 2.0);
  EXPECT_NE(got.find("408"), std::string::npos);
  EXPECT_EQ(rig.backends[0]->hits(), 0u);
}

TEST(Integration, TricklingBytesDoesNotExtendTheHeaderDeadline) {
  Rig rig(1, [](Config& c) { c.header_timeout = 0.6; });
  int fd = connect_to(rig.port());
  ASSERT_GE(fd, 0);
  auto start = std::chrono::steady_clock::now();
  bool closed = false;
  for (int i = 0; i < 30 && !closed; ++i) {  // one byte every 100ms for up to 3s
    ssize_t n = send(fd, "G", 1, MSG_NOSIGNAL);
    if (n <= 0) closed = true;
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, 100) > 0) closed = true;  // server sent 408 / closed
  }
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  close(fd);
  EXPECT_TRUE(closed);
  EXPECT_LT(elapsed, 2.0);
}

TEST(Integration, ConnectionLimitIsEnforced) {
  Rig rig(1, [](Config& c) { c.max_conns = 3; });
  std::vector<int> fds;
  for (int i = 0; i < 3; ++i) {
    int fd = connect_to(rig.port());
    ASSERT_GE(fd, 0);
    fds.push_back(fd);
  }
  ASSERT_TRUE(wait_for([&] { return metric(rig.metrics(), "proxy_active_connections") >= 3; }, 2));
  int extra = connect_to(rig.port());  // accepted by the kernel, then shed by the proxy
  ASSERT_GE(extra, 0);
  char c;
  EXPECT_LE(recv(extra, &c, 1, 0), 0);
  EXPECT_GE(metric(rig.metrics(), "proxy_connections_rejected_total"), 1);
  close(extra);
  for (int fd : fds) close(fd);
}

// ---------------------------------------------------------------------------
// Caching and coalescing
// ---------------------------------------------------------------------------

TEST(Integration, SecondRequestIsServedFromCache) {
  Rig rig(1);
  auto first = get(rig.port(), "/page");
  auto second = get(rig.port(), "/page");
  ASSERT_TRUE(first.ok && second.ok);
  EXPECT_EQ(first.headers["x-cache"], "MISS");
  EXPECT_EQ(second.headers["x-cache"], "HIT");
  EXPECT_EQ(first.body, second.body);
  EXPECT_EQ(rig.backends[0]->hits_for("/page"), 1u);
  EXPECT_TRUE(second.headers.count("age"));
}

TEST(Integration, CacheEntriesExpire) {
  Rig rig(1);
  ASSERT_EQ(get(rig.port(), "/ttl?s=1").headers["x-cache"], "MISS");
  EXPECT_EQ(get(rig.port(), "/ttl?s=1").headers["x-cache"], "HIT");
  std::this_thread::sleep_for(std::chrono::milliseconds(1300));
  EXPECT_EQ(get(rig.port(), "/ttl?s=1").headers["x-cache"], "MISS");
  EXPECT_EQ(rig.backends[0]->hits_for("/ttl"), 2u);
}

TEST(Integration, ResponsesThatMustNotBeCachedAreNot) {
  Rig rig(1);
  for (const char* path : {"/nocache", "/cookie", "/status/404", "/status/500"}) {
    get(rig.port(), path);
    get(rig.port(), path);
    EXPECT_EQ(rig.backends[0]->hits_for(path), 2u) << path;
  }
}

TEST(Integration, PersonalisedRequestsBypassTheCache) {
  Rig rig(1);
  auto a = get(rig.port(), "/account", "Authorization: Bearer x\r\n");
  auto b = get(rig.port(), "/account", "Cookie: sid=1\r\n");
  auto c = get(rig.port(), "/account", "Range: bytes=0-3\r\n");
  EXPECT_EQ(a.headers["x-cache"], "BYPASS");
  EXPECT_EQ(b.headers["x-cache"], "BYPASS");
  EXPECT_EQ(c.headers["x-cache"], "BYPASS");
  EXPECT_EQ(rig.backends[0]->hits_for("/account"), 3u);
  // ...and nothing was stored for anonymous visitors to receive.
  EXPECT_EQ(get(rig.port(), "/account").headers["x-cache"], "MISS");
}

TEST(Integration, ConcurrentMissesAreCoalescedIntoOneOriginRequest) {
  Rig rig(1, nullptr, /*backend_delay_ms=*/400);
  constexpr int kClients = 100;
  std::vector<HttpResult> results(kClients);
  std::vector<std::thread> threads;
  for (int i = 0; i < kClients; ++i)
    threads.emplace_back([&, i] { results[i] = get(rig.port(), "/popular"); });
  for (auto& t : threads) t.join();

  int miss = 0, coalesced = 0, hit = 0;
  for (auto& r : results) {
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "backend:a path:/popular\n");
    if (r.headers["x-cache"] == "MISS") ++miss;
    else if (r.headers["x-cache"] == "COALESCED") ++coalesced;
    else if (r.headers["x-cache"] == "HIT") ++hit;
  }
  EXPECT_EQ(rig.backends[0]->hits_for("/popular"), 1u);  // the whole point
  EXPECT_EQ(miss, 1);
  EXPECT_EQ(miss + coalesced + hit, kClients);
  EXPECT_GT(coalesced, kClients / 2);
}

TEST(Integration, CoalescedWaitersOfUncacheableResponsesAreNotGivenSomeoneElsesCopy) {
  Rig rig(1, nullptr, /*backend_delay_ms=*/200);
  constexpr int kClients = 8;
  std::vector<HttpResult> results(kClients);
  std::vector<std::thread> threads;
  for (int i = 0; i < kClients; ++i)
    threads.emplace_back([&, i] { results[i] = get(rig.port(), "/nocache"); });
  for (auto& t : threads) t.join();
  for (auto& r : results) {
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 200);
    EXPECT_NE(r.headers["x-cache"], "HIT");
    EXPECT_NE(r.headers["x-cache"], "COALESCED");
  }
  EXPECT_EQ(rig.backends[0]->hits_for("/nocache"), static_cast<uint64_t>(kClients));
}

TEST(Integration, CacheDisabledMeansEveryRequestHitsTheOrigin) {
  Rig rig(1, [](Config& c) { c.cache_enabled = false; });
  for (int i = 0; i < 4; ++i) get(rig.port(), "/same");
  EXPECT_EQ(rig.backends[0]->hits_for("/same"), 4u);
}

// ---------------------------------------------------------------------------
// Load balancing, failover, health
// ---------------------------------------------------------------------------

TEST(Integration, RoundRobinSpreadsLoadEvenly) {
  Rig rig(3, [](Config& c) { c.cache_enabled = false; });
  for (int i = 0; i < 30; ++i) ASSERT_EQ(get(rig.port(), "/rr").status, 200);
  for (auto& b : rig.backends) EXPECT_EQ(b->hits(), 10u);
}

TEST(Integration, ConsistentHashingPinsAUrlToOneBackend) {
  Rig rig(3, [](Config& c) {
    c.cache_enabled = false;
    c.policy = Balancer::Policy::ConsistentHash;
  });
  std::set<std::string> seen;
  for (int i = 0; i < 10; ++i) seen.insert(get(rig.port(), "/sticky").headers["x-backend"]);
  EXPECT_EQ(seen.size(), 1u);
  std::set<std::string> spread;
  for (int i = 0; i < 60; ++i) spread.insert(get(rig.port(), "/p" + std::to_string(i)).headers["x-backend"]);
  EXPECT_GE(spread.size(), 2u);
}

TEST(Integration, RequestsFailOverWhenABackendDiesMidTraffic) {
  Rig rig(3, [](Config& c) { c.cache_enabled = false; });
  for (int i = 0; i < 6; ++i) ASSERT_EQ(get(rig.port(), "/warm").status, 200);
  rig.backends[1]->stop();  // crash one origin
  int failures = 0;
  for (int i = 0; i < 60; ++i)
    if (get(rig.port(), "/after").status != 200) ++failures;
  EXPECT_EQ(failures, 0);  // retried transparently on another backend
  std::string m = rig.metrics();
  EXPECT_GE(metric(m, "proxy_upstream_retries_total"), 1);
  EXPECT_EQ(metric(m, "proxy_backend_up{backend=\"" + rig.backends[1]->address() + "\"}"), 0);  // passive check ejected it
}

TEST(Integration, ActiveHealthChecksEjectAndRestoreABackend) {
  Rig rig(2, [](Config& c) {
    c.cache_enabled = false;
    c.health.interval = 0.1;
    c.health.timeout = 0.3;
    c.health.fall = 2;
    c.health.rise = 2;
  });
  const std::string up_metric = "proxy_backend_up{backend=\"" + rig.backends[0]->address() + "\"}";
  ASSERT_TRUE(wait_for([&] { return metric(rig.metrics(), up_metric) == 1; }, 2));
  rig.backends[0]->set_healthy(false);
  ASSERT_TRUE(wait_for([&] { return metric(rig.metrics(), up_metric) == 0; }, 3)) << "backend was never ejected";
  for (int i = 0; i < 10; ++i) EXPECT_EQ(get(rig.port(), "/x").headers["x-backend"], "b");  // traffic avoids it
  rig.backends[0]->set_healthy(true);
  ASSERT_TRUE(wait_for([&] { return metric(rig.metrics(), up_metric) == 1; }, 3)) << "backend never came back";
  std::set<std::string> seen;
  for (int i = 0; i < 20; ++i) seen.insert(get(rig.port(), "/y").headers["x-backend"]);
  EXPECT_EQ(seen.size(), 2u);
}

TEST(Integration, AllBackendsDownGives502) {
  Rig rig(2, [](Config& c) { c.cache_enabled = false; });
  rig.backends[0]->stop();
  rig.backends[1]->stop();
  auto r = get(rig.port(), "/x");
  EXPECT_EQ(r.status, 502);
  EXPECT_EQ(r.body, "upstream failure\n");
}

TEST(Integration, SlowOriginsGet504AndTheClientIsNotLeftHanging) {
  Rig rig(1, [](Config& c) {
    c.upstream_timeout = 0.3;
    c.max_attempts = 1;
  }, /*backend_delay_ms=*/1500);
  auto start = std::chrono::steady_clock::now();
  auto r = get(rig.port(), "/slow");
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  EXPECT_EQ(r.status, 504);
  EXPECT_LT(elapsed, 1.2);
  EXPECT_GE(metric(rig.metrics(), "proxy_upstream_timeouts_total"), 1);
}

TEST(Integration, OversizedOriginResponsesGet502) {
  Rig rig(1, [](Config& c) { c.max_response_bytes = 1000; });
  EXPECT_EQ(get(rig.port(), "/big?n=5000").status, 502);
  EXPECT_EQ(get(rig.port(), "/big?n=500").status, 200);
}

// ---------------------------------------------------------------------------
// Rate limiting and metrics
// ---------------------------------------------------------------------------

TEST(Integration, RateLimitReturns429WithRetryAfter) {
  Rig rig(1, [](Config& c) {
    c.cache_enabled = false;
    c.rate_per_sec = 5;
    c.burst = 5;
  });
  int ok = 0, limited = 0;
  HttpResult last_limited;
  for (int i = 0; i < 15; ++i) {
    auto r = get(rig.port(), "/rl");
    if (r.status == 200) ++ok;
    if (r.status == 429) {
      ++limited;
      last_limited = r;
    }
  }
  EXPECT_GE(ok, 5);
  EXPECT_LE(ok, 8);  // the burst, plus a little refill while the loop runs
  EXPECT_GE(limited, 7);
  EXPECT_TRUE(last_limited.headers.count("retry-after"));
  EXPECT_LT(rig.backends[0]->hits(), 9u);  // rejected requests never reach the origin
  EXPECT_GE(metric(rig.metrics(), "proxy_rate_limited_total"), 7);
}

TEST(Integration, MetricsEndpointExposesPrometheusText) {
  Rig rig(1);
  get(rig.port(), "/m");
  get(rig.port(), "/m");
  std::string m = rig.metrics();
  EXPECT_GE(metric(m, "proxy_requests_total"), 2);
  EXPECT_EQ(metric(m, "proxy_cache_hits_total"), 1);
  EXPECT_EQ(metric(m, "proxy_cache_misses_total"), 1);
  EXPECT_EQ(metric(m, "proxy_responses_total{class=\"2xx\"}"), 2);
  EXPECT_NE(m.find("# TYPE proxy_request_duration_seconds histogram"), std::string::npos);
  EXPECT_NE(m.find("proxy_request_duration_seconds_bucket{le=\"+Inf\"}"), std::string::npos);
  EXPECT_GE(metric(m, "proxy_request_duration_seconds_count"), 2);
}

TEST(Integration, AdminPortOnlyServesGetOnItsOwnPaths) {
  Rig rig(1);
  EXPECT_EQ(http(rig.proxy->admin_port(), "GET", "/healthz").status, 200);
  EXPECT_EQ(http(rig.proxy->admin_port(), "GET", "/other").status, 404);
  EXPECT_EQ(http(rig.proxy->admin_port(), "POST", "/metrics").status, 405);
  // The public port does not expose metrics: /metrics is just another URL for the origin.
  EXPECT_EQ(get(rig.port(), "/metrics").body, "backend:a path:/metrics\n");
}

TEST(Integration, ClientsThatDisconnectEarlyDoNotBreakTheProxy) {
  Rig rig(1, nullptr, /*backend_delay_ms=*/150);
  for (int i = 0; i < 20; ++i) {
    int fd = connect_to(rig.port());
    ASSERT_GE(fd, 0);
    send_all(fd, "GET /gone" + std::to_string(i % 3) + " HTTP/1.1\r\nHost: h\r\n\r\n");
    close(fd);  // leave before the response
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_EQ(get(rig.port(), "/still-alive").status, 200);
}
