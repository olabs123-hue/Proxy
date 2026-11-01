#include "proxy.h"

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

#include "util.h"

namespace edge {

namespace {

constexpr uint32_t kIn = EPOLLIN;
constexpr uint32_t kOut = EPOLLOUT;
constexpr uint32_t kErr = EPOLLERR | EPOLLHUP;

std::shared_ptr<Response> make_text_response(int status, const std::string& body, std::vector<Header> extra = {}) {
  auto r = std::make_shared<Response>();
  r->status = status;
  r->reason = default_reason(status);
  r->headers.push_back({"Content-Type", "text/plain; charset=utf-8"});
  for (auto& h : extra) r->headers.push_back(std::move(h));
  r->body = body;
  r->declared_length = body.size();
  return r;
}

int listen_on(const std::string& addr, int port, uint16_t* bound_port, std::string* err) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    *err = std::string("socket: ") + strerror(errno);
    return -1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) {
    *err = "invalid bind address '" + addr + "'";
    close(fd);
    return -1;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
    *err = "bind " + addr + ":" + std::to_string(port) + ": " + strerror(errno);
    close(fd);
    return -1;
  }
  if (listen(fd, 1024) != 0) {
    *err = std::string("listen: ") + strerror(errno);
    close(fd);
    return -1;
  }
  socklen_t len = sizeof(sa);
  getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len);
  *bound_port = ntohs(sa.sin_port);
  return fd;
}

}  // namespace

Proxy::Proxy(Config cfg)
    : cfg_(std::move(cfg)),
      cache_(cfg_.cache_bytes, cfg_.cache_max_object),
      limiter_(cfg_.rate_per_sec, cfg_.burst > 0 ? cfg_.burst : cfg_.rate_per_sec * 2) {}

Proxy::~Proxy() {
  if (health_) health_->stop();
  for (auto& kv : conns_)
    if (kv.second->fd >= 0) close(kv.second->fd);
  for (auto& kv : fetches_)
    if (kv.second->fd >= 0) close(kv.second->fd);
  if (listen_fd_ >= 0) close(listen_fd_);
  if (admin_fd_ >= 0) close(admin_fd_);
  if (spare_fd_ >= 0) close(spare_fd_);
}

bool Proxy::init(std::string* err) {
  signal(SIGPIPE, SIG_IGN);
  if (cfg_.backends.empty()) {
    *err = "at least one --backend is required";
    return false;
  }
  std::vector<std::unique_ptr<Backend>> backends;
  for (const auto& spec : cfg_.backends) {
    auto b = make_backend(spec, err);
    if (!b) return false;
    backends.push_back(std::move(b));
  }
  balancer_ = std::make_unique<Balancer>(std::move(backends), cfg_.policy);

  listen_fd_ = listen_on(cfg_.bind_addr, cfg_.listen_port, &port_, err);
  if (listen_fd_ < 0) return false;
  loop_.add(listen_fd_, kIn, [this](uint32_t) { on_accept(listen_fd_, false); });
  if (cfg_.admin_port >= 0) {
    admin_fd_ = listen_on(cfg_.admin_bind, cfg_.admin_port, &admin_port_, err);
    if (admin_fd_ < 0) return false;
    loop_.add(admin_fd_, kIn, [this](uint32_t) { on_accept(admin_fd_, true); });
  }
  spare_fd_ = open("/dev/null", O_RDONLY | O_CLOEXEC);

  loop_.add_timer(0.2, [this] { sweep_timeouts(); });
  loop_.add_timer(1.0, [this] { sweep_housekeeping(); });
  health_ = std::make_unique<HealthChecker>(*balancer_, cfg_.health);
  return true;
}

void Proxy::run() {
  health_->start();
  loop_.run();
  health_->stop();
}

void Proxy::stop() { loop_.stop(); }

// ---------------------------------------------------------------------------
// Accepting connections
// ---------------------------------------------------------------------------

void Proxy::on_accept(int lfd, bool admin) {
  for (int i = 0; i < 256; ++i) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    int fd = accept4(lfd, reinterpret_cast<sockaddr*>(&peer), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (errno == EMFILE || errno == ENFILE) {
        // Out of descriptors: free the spare, accept-and-close to shed the
        // backlog entry (otherwise the listener spins), then re-reserve.
        if (spare_fd_ >= 0) {
          close(spare_fd_);
          int shed = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
          if (shed >= 0) close(shed);
          spare_fd_ = open("/dev/null", O_RDONLY | O_CLOEXEC);
          ++metrics_.connections_rejected;
        }
      }
      break;  // EAGAIN or unrecoverable
    }
    if (!admin && conns_.size() >= cfg_.max_conns) {
      close(fd);
      ++metrics_.connections_rejected;
      continue;
    }
    set_nodelay(fd);
    auto c = std::make_unique<ClientConn>();
    c->id = next_id_++;
    c->fd = fd;
    c->ip = ntohl(peer.sin_addr.s_addr);
    c->admin = admin;
    c->deadline = now_seconds() + cfg_.idle_timeout;
    uint64_t id = c->id;
    if (!loop_.add(fd, kIn, [this, id](uint32_t ev) { on_client_event(id, ev); })) {
      close(fd);
      continue;
    }
    conns_[id] = std::move(c);
    ++metrics_.connections_accepted;
  }
}

Proxy::ClientConn* Proxy::find_client(uint64_t id) {
  auto it = conns_.find(id);
  if (it == conns_.end() || it->second->closed) return nullptr;
  return it->second.get();
}

void Proxy::close_client(ClientConn& c) {
  if (c.closed) return;
  c.closed = true;
  loop_.remove(c.fd);
  close(c.fd);
  c.fd = -1;
  uint64_t id = c.id;
  // The object may still be on the call stack; destroy it after dispatch.
  loop_.defer([this, id] { conns_.erase(id); });
}

void Proxy::on_client_event(uint64_t id, uint32_t events) {
  ClientConn* c = find_client(id);
  if (!c) return;
  if (events & kErr) {
    close_client(*c);
    return;
  }
  if ((events & kIn) && c->state == ClientConn::State::ReadingRequest) {
    client_read(*c);
    return;
  }
  if ((events & kOut) && c->state == ClientConn::State::Writing) client_write(*c);
}

void Proxy::client_read(ClientConn& c) {
  char buf[16384];
  ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
  if (n > 0) {
    c.in.append(buf, static_cast<size_t>(n));
    if (!c.partial) {
      // Deadline starts at the first byte of a request and is NOT extended by
      // later bytes, which defeats slow-header (slowloris) clients.
      c.partial = true;
      c.deadline = now_seconds() + cfg_.header_timeout;
    }
    process_requests(c);
  } else if (n == 0) {
    close_client(c);
  } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    close_client(c);
  }
}

void Proxy::process_requests(ClientConn& c) {
  if (c.processing) return;  // an outer frame is already looping
  c.processing = true;
  RequestLimits limits;
  limits.max_header_bytes = cfg_.max_header_bytes;
  while (!c.closed && c.state == ClientConn::State::ReadingRequest && !c.in.empty()) {
    RequestParseResult r = parse_request(c.in.data(), c.in.size(), limits);
    if (r.status == ParseStatus::NeedMore) break;
    if (r.status == ParseStatus::Error) {
      ++metrics_.bad_requests;
      c.req_start = now_seconds();
      c.req = Request{};
      c.in.clear();
      deliver_generated(c, r.error_status, r.error_message + "\n", false);
      break;
    }
    c.in.erase(0, r.consumed);
    c.partial = !c.in.empty();
    handle_request(c, std::move(r.request));
  }
  c.processing = false;
}

// ---------------------------------------------------------------------------
// Request handling
// ---------------------------------------------------------------------------

bool Proxy::cache_eligible(const Request& req) const {
  static const char* kBlockers[] = {"authorization", "cookie",        "range",         "if-match",
                                    "if-none-match", "if-modified-since", "if-unmodified-since", "if-range"};
  for (const char* h : kBlockers)
    if (find_header(req.headers, h)) return false;
  if (const std::string* cc = find_header(req.headers, "cache-control")) {
    std::string l = to_lower(*cc);
    if (l.find("no-cache") != std::string::npos || l.find("no-store") != std::string::npos) return false;
  }
  return true;
}

bool Proxy::is_cacheable(const Response& r, double* ttl) const {
  if (r.status != 200) return false;
  if (find_header(r.headers, "set-cookie")) return false;
  if (find_header(r.headers, "vary")) return false;  // we do not implement Vary; be safe
  CacheControl cc = parse_cache_control(r.headers);
  if (cc.no_store || cc.no_cache || cc.is_private) return false;
  double t = cfg_.cache_ttl;
  if (cc.s_maxage) t = static_cast<double>(*cc.s_maxage);
  else if (cc.max_age) t = static_cast<double>(*cc.max_age);
  if (t <= 0) return false;
  *ttl = t;
  return true;
}

void Proxy::handle_request(ClientConn& c, Request&& req) {
  c.req = std::move(req);
  c.req_start = now_seconds();
  ++metrics_.requests_total;
  if (c.admin) {
    handle_admin(c);
    return;
  }
  bool keep = c.req.keep_alive;
  if (c.req.method != "GET" && c.req.method != "HEAD") {
    deliver_generated(c, 405, "method not allowed\n", keep, {{"Allow", "GET, HEAD"}});
    return;
  }
  RateLimiter::Decision d = limiter_.allow(c.ip, c.req_start);
  if (!d.allowed) {
    ++metrics_.rate_limited;
    long retry = std::max(1L, static_cast<long>(std::ceil(d.retry_after)));
    deliver_generated(c, 429, "rate limit exceeded\n", keep, {{"Retry-After", std::to_string(retry)}});
    return;
  }
  dispatch(c);
}

void Proxy::handle_admin(ClientConn& c) {
  if (c.req.method != "GET") {
    deliver_generated(c, 405, "method not allowed\n", false, {{"Allow", "GET"}});
  } else if (c.req.target == "/metrics") {
    auto r = make_text_response(200, metrics_.render(*balancer_, cache_, conns_.size(), fetches_.size()),
                                {{"Cache-Control", "no-store"}});
    r->headers[0].value = "text/plain; version=0.0.4";
    deliver(c, r, nullptr, -1);
    c.close_after = true;
  } else if (c.req.target == "/healthz") {
    deliver_generated(c, 200, "ok\n", false);
  } else {
    deliver_generated(c, 404, "not found\n", false);
  }
}

void Proxy::dispatch(ClientConn& c) {
  double now = now_seconds();
  bool is_head = c.req.method == "HEAD";
  std::string key = to_lower(c.req.host) + c.req.target;

  bool eligible = cfg_.cache_enabled && !is_head && cache_eligible(c.req);
  if (eligible) {
    auto p = pass_until_.find(key);
    if (p != pass_until_.end()) {
      if (now < p->second) eligible = false;
      else pass_until_.erase(p);
    }
  }

  if (eligible) {
    if (auto hit = cache_.get(key, now)) {
      ++metrics_.cache_hits;
      deliver(c, hit, "HIT", static_cast<long>(now - hit->stored_at));
      return;
    }
    auto it = inflight_.find(key);
    if (it != inflight_.end()) {
      auto f = fetches_.find(it->second);
      if (f != fetches_.end()) {
        // Someone is already fetching this object: wait for that fetch.
        f->second->waiters.push_back(c.id);
        c.state = ClientConn::State::WaitingUpstream;
        c.deadline = 0;
        loop_.modify(c.fd, 0);
        ++metrics_.cache_coalesced;
        return;
      }
    }
    ++metrics_.cache_misses;
  } else {
    ++metrics_.cache_bypass;
  }

  auto f = std::make_unique<Fetch>();
  f->id = next_id_++;
  f->eligible = eligible;
  f->is_head = is_head;
  f->cache_key = key;
  f->request_bytes = build_upstream_request(c.req, ip_to_string(c.ip));
  f->waiters.push_back(c.id);
  uint64_t fid = f->id;
  if (eligible) inflight_[key] = fid;
  fetches_[fid] = std::move(f);
  c.state = ClientConn::State::WaitingUpstream;
  c.deadline = 0;
  loop_.modify(c.fd, 0);
  start_attempt(fid);
}

// ---------------------------------------------------------------------------
// Writing responses to clients
// ---------------------------------------------------------------------------

void Proxy::deliver_generated(ClientConn& c, int status, const std::string& body, bool keep_alive,
                              std::vector<Header> extra) {
  auto r = make_text_response(status, body, std::move(extra));
  c.close_after = !keep_alive;
  deliver(c, r, nullptr, -1);
}

void Proxy::deliver(ClientConn& c, std::shared_ptr<const Response> resp, const char* x_cache, long age) {
  if (c.closed) return;
  bool is_head = c.req.method == "HEAD";
  bool keep = !c.close_after && c.req.keep_alive;
  c.close_after = !keep;
  c.out_head = serialize_response_head(*resp, is_head, keep, x_cache, age);
  c.out_body = is_head ? nullptr : std::shared_ptr<const std::string>(resp, &resp->body);  // aliasing ctor
  c.head_off = 0;
  c.body_off = 0;
  c.state = ClientConn::State::Writing;
  c.deadline = now_seconds() + cfg_.write_timeout;
  metrics_.count_response(resp->status);
  client_write(c);
}

void Proxy::client_write(ClientConn& c) {
  for (;;) {
    iovec iov[2];
    int cnt = 0;
    if (c.head_off < c.out_head.size()) {
      iov[cnt].iov_base = const_cast<char*>(c.out_head.data()) + c.head_off;
      iov[cnt].iov_len = c.out_head.size() - c.head_off;
      ++cnt;
    }
    size_t body_size = c.out_body ? c.out_body->size() : 0;
    if (c.body_off < body_size) {
      iov[cnt].iov_base = const_cast<char*>(c.out_body->data()) + c.body_off;
      iov[cnt].iov_len = body_size - c.body_off;
      ++cnt;
    }
    if (cnt == 0) break;
    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = cnt;
    ssize_t n = sendmsg(c.fd, &msg, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        loop_.modify(c.fd, kOut);  // backpressure: wait until the client drains
        return;
      }
      close_client(c);
      return;
    }
    size_t left = static_cast<size_t>(n);
    size_t head_left = c.out_head.size() - c.head_off;
    size_t used_head = std::min(left, head_left);
    c.head_off += used_head;
    c.body_off += left - used_head;
  }
  finish_response(c);
}

void Proxy::finish_response(ClientConn& c) {
  metrics_.observe_latency(now_seconds() - c.req_start);
  c.out_body.reset();
  c.out_head.clear();
  c.head_off = c.body_off = 0;
  if (c.close_after) {
    close_client(c);
    return;
  }
  c.state = ClientConn::State::ReadingRequest;
  c.deadline = now_seconds() + (c.in.empty() ? cfg_.idle_timeout : cfg_.header_timeout);
  loop_.modify(c.fd, kIn);
  if (!c.in.empty()) process_requests(c);  // pipelined requests already buffered
}

// ---------------------------------------------------------------------------
// Upstream fetches
// ---------------------------------------------------------------------------

void Proxy::close_fetch_fd(Fetch& f) {
  if (f.fd >= 0) {
    loop_.remove(f.fd);
    close(f.fd);
    f.fd = -1;
  }
}

void Proxy::start_attempt(uint64_t fid) {
  auto it = fetches_.find(fid);
  if (it == fetches_.end()) return;
  Fetch& f = *it->second;
  Backend* b = balancer_->pick(f.cache_key, f.tried);
  if (!b) {
    fail_fetch(fid, f.tried.empty() ? 503 : (f.last_timeout ? 504 : 502),
               f.tried.empty() ? "no upstream available\n" : "upstream failure\n");
    return;
  }
  if (!f.tried.empty()) ++metrics_.upstream_retries;
  f.backend = b;
  f.tried.push_back(b);
  ++metrics_.upstream_requests;

  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    attempt_failed(fid, "socket() failed", false);
    return;
  }
  set_nodelay(fd);
  int rc = connect(fd, reinterpret_cast<const sockaddr*>(&b->addr), sizeof(b->addr));
  if (rc < 0 && errno != EINPROGRESS) {
    close(fd);
    attempt_failed(fid, strerror(errno), false);
    return;
  }
  f.fd = fd;
  f.connecting = true;
  f.sent = 0;
  f.parser = std::make_unique<ResponseParser>(f.is_head, cfg_.max_response_bytes);
  f.attempt_start = now_seconds();
  f.deadline = f.attempt_start + cfg_.connect_timeout;
  if (!loop_.add(fd, kOut, [this, fid](uint32_t ev) { on_fetch_event(fid, ev); })) {
    close(fd);
    f.fd = -1;
    attempt_failed(fid, "epoll registration failed", false);
  }
}

void Proxy::on_fetch_event(uint64_t fid, uint32_t events) {
  auto it = fetches_.find(fid);
  if (it == fetches_.end()) return;
  Fetch& f = *it->second;

  if (f.connecting) {
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(f.fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
      attempt_failed(fid, strerror(err), false);
      return;
    }
    if (!(events & (kOut | kErr))) return;
    f.connecting = false;
    f.deadline = f.attempt_start + cfg_.upstream_timeout;
  }

  if (f.sent < f.request_bytes.size()) {
    while (f.sent < f.request_bytes.size()) {
      ssize_t n = send(f.fd, f.request_bytes.data() + f.sent, f.request_bytes.size() - f.sent, MSG_NOSIGNAL);
      if (n > 0) {
        f.sent += static_cast<size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;  // still registered for EPOLLOUT
      } else {
        attempt_failed(fid, strerror(errno), false);
        return;
      }
    }
    loop_.modify(f.fd, kIn);
    return;  // level-triggered: EPOLLIN will fire when the response arrives
  }

  if (events & (kIn | kErr)) {
    char buf[16384];
    for (int i = 0; i < 4; ++i) {
      ssize_t n = recv(f.fd, buf, sizeof(buf), 0);
      if (n > 0) {
        ParseStatus st = f.parser->feed(buf, static_cast<size_t>(n));
        if (st == ParseStatus::Done) {
          complete_fetch(fid);
          return;
        }
        if (st == ParseStatus::Error) {
          if (f.parser->too_large()) {
            close_fetch_fd(f);
            fail_fetch(fid, 502, "upstream response too large\n");
          } else {
            attempt_failed(fid, f.parser->error().c_str(), false);
          }
          return;
        }
      } else if (n == 0) {
        if (f.parser->on_eof() == ParseStatus::Done) complete_fetch(fid);
        else attempt_failed(fid, f.parser->error().c_str(), false);
        return;
      } else if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      } else {
        attempt_failed(fid, strerror(errno), false);
        return;
      }
    }
  }
}

void Proxy::attempt_failed(uint64_t fid, const char* reason, bool timeout) {
  auto it = fetches_.find(fid);
  if (it == fetches_.end()) return;
  Fetch& f = *it->second;
  log_msg(1, "upstream %s failed: %s", f.backend ? f.backend->name.c_str() : "?", reason);
  close_fetch_fd(f);
  ++metrics_.upstream_errors;
  if (timeout) ++metrics_.upstream_timeouts;
  f.last_timeout = timeout;
  if (f.backend) balancer_->report_failure(f.backend, cfg_.passive_fail_threshold);
  if (static_cast<int>(f.tried.size()) < cfg_.max_attempts) {
    start_attempt(fid);  // safe to retry: GET/HEAD are idempotent and nothing was sent to the client
    return;
  }
  fail_fetch(fid, timeout ? 504 : 502, timeout ? "upstream timeout\n" : "upstream failure\n");
}

void Proxy::complete_fetch(uint64_t fid) {
  auto it = fetches_.find(fid);
  if (it == fetches_.end()) return;
  std::unique_ptr<Fetch> f = std::move(it->second);
  fetches_.erase(it);
  close_fetch_fd(*f);
  balancer_->report_success(f->backend);

  auto resp = std::make_shared<Response>(std::move(f->parser->response()));
  double now = now_seconds();
  resp->stored_at = now;
  double ttl = 0;
  bool cacheable = f->eligible && is_cacheable(*resp, &ttl);
  std::shared_ptr<const Response> cresp = resp;
  if (cacheable) cache_.put(f->cache_key, cresp, ttl, now);
  finish_fetch(std::move(f), cresp, false, cacheable, nullptr);
}

void Proxy::fail_fetch(uint64_t fid, int status, const std::string& msg) {
  auto it = fetches_.find(fid);
  if (it == fetches_.end()) return;
  std::unique_ptr<Fetch> f = std::move(it->second);
  fetches_.erase(it);
  close_fetch_fd(*f);
  auto resp = make_text_response(status, msg);
  finish_fetch(std::move(f), resp, true, false, nullptr);
}

// Hands a finished fetch's result to the clients waiting on it.
//  share_with_all: give every waiter this response (used for generated errors
//                  so a failing origin is not hammered by every waiter).
//  cacheable:      the response was stored, so sharing it is safe.
// Otherwise (an uncacheable response to a coalesced fetch) only the leader
// gets it; the rest are re-dispatched as independent requests, because the
// response might be personalised.
void Proxy::finish_fetch(std::unique_ptr<Fetch> f, std::shared_ptr<const Response> resp, bool share_with_all,
                         bool cacheable, const char*) {
  auto inf = inflight_.find(f->cache_key);
  if (inf != inflight_.end() && inf->second == f->id) inflight_.erase(inf);
  if (f->eligible && !cacheable && !share_with_all) {
    if (pass_until_.size() > 10000) pass_until_.clear();
    pass_until_[f->cache_key] = now_seconds() + cfg_.pass_ttl;
  }
  std::vector<uint64_t> waiters = std::move(f->waiters);
  f.reset();
  for (size_t i = 0; i < waiters.size(); ++i) {
    ClientConn* c = find_client(waiters[i]);
    if (!c || c->state != ClientConn::State::WaitingUpstream) continue;
    if (i == 0) {
      deliver(*c, resp, share_with_all ? nullptr : (cacheable ? "MISS" : "BYPASS"), -1);
    } else if (cacheable || share_with_all) {
      deliver(*c, resp, share_with_all ? nullptr : "COALESCED", -1);
    } else {
      c->state = ClientConn::State::ReadingRequest;  // dispatch() will set it again
      dispatch(*c);
    }
  }
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

void Proxy::sweep_timeouts() {
  double now = now_seconds();
  std::vector<uint64_t> expired_clients;
  for (auto& kv : conns_) {
    ClientConn& c = *kv.second;
    if (!c.closed && c.deadline > 0 && now > c.deadline) expired_clients.push_back(c.id);
  }
  for (uint64_t id : expired_clients) {
    ClientConn* c = find_client(id);
    if (!c) continue;
    if (c->state == ClientConn::State::ReadingRequest && c->partial) {
      static const char kTimeout[] =
          "HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      ssize_t r = send(c->fd, kTimeout, sizeof(kTimeout) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
      (void)r;
    }
    close_client(*c);
  }
  std::vector<uint64_t> expired_fetches;
  for (auto& kv : fetches_)
    if (kv.second->fd >= 0 && kv.second->deadline > 0 && now > kv.second->deadline) expired_fetches.push_back(kv.first);
  for (uint64_t id : expired_fetches) attempt_failed(id, "timeout", true);
}

void Proxy::sweep_housekeeping() {
  double now = now_seconds();
  cache_.purge_expired(now);
  limiter_.sweep(now);
  for (auto it = pass_until_.begin(); it != pass_until_.end();) {
    if (now >= it->second) it = pass_until_.erase(it);
    else ++it;
  }
}

}  // namespace edge
