#pragma once
// The edge proxy: a single-threaded, epoll-driven HTTP/1.1 reverse proxy with
// load balancing, health checking, rate limiting, response caching with
// request coalescing, and Prometheus metrics.
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "balancer.h"
#include "cache.h"
#include "event_loop.h"
#include "health_checker.h"
#include "http.h"
#include "metrics.h"
#include "rate_limiter.h"

namespace edge {

struct Config {
  std::string bind_addr = "127.0.0.1";
  uint16_t listen_port = 8080;  // 0 = pick an ephemeral port
  int admin_port = 9100;        // -1 disables, 0 = ephemeral. Always bound to admin_bind.
  std::string admin_bind = "127.0.0.1";

  std::vector<std::string> backends;  // "host:port"
  Balancer::Policy policy = Balancer::Policy::RoundRobin;

  double rate_per_sec = 0;  // per client IP; 0 disables
  double burst = 0;

  bool cache_enabled = true;
  size_t cache_bytes = 64 * 1024 * 1024;
  size_t cache_max_object = 4 * 1024 * 1024;
  double cache_ttl = 30;   // default TTL when the origin gives no max-age
  double pass_ttl = 5;     // how long an uncacheable URL skips coalescing

  double header_timeout = 10;  // time allowed to deliver a full request head
  double idle_timeout = 30;    // keep-alive idle time
  double write_timeout = 30;   // time allowed to drain a response to a slow client
  double connect_timeout = 2;
  double upstream_timeout = 10;  // whole attempt, connect included

  size_t max_conns = 10000;
  size_t max_header_bytes = 16 * 1024;
  size_t max_response_bytes = 16 * 1024 * 1024;
  int max_attempts = 2;           // tries across distinct backends (GET/HEAD are idempotent)
  int passive_fail_threshold = 3; // consecutive proxy failures that mark a backend down

  HealthConfig health;
};

class Proxy {
 public:
  explicit Proxy(Config cfg);
  ~Proxy();
  Proxy(const Proxy&) = delete;
  Proxy& operator=(const Proxy&) = delete;

  bool init(std::string* err);
  uint16_t port() const { return port_; }
  uint16_t admin_port() const { return admin_port_; }

  void run();   // blocks until stop()
  void stop();  // thread-safe, async-signal-safe

  Balancer& balancer() { return *balancer_; }

 private:
  struct ClientConn {
    enum class State { ReadingRequest, WaitingUpstream, Writing };
    uint64_t id = 0;
    int fd = -1;
    uint32_t ip = 0;  // host byte order
    bool admin = false;
    bool closed = false;
    bool processing = false;
    bool partial = false;  // bytes of an unfinished request are buffered
    State state = State::ReadingRequest;
    std::string in;
    double deadline = 0;  // absolute; 0 = none
    double req_start = 0;
    Request req;
    std::string out_head;
    std::shared_ptr<const std::string> out_body;
    size_t head_off = 0;
    size_t body_off = 0;
    bool close_after = false;
  };

  struct Fetch {
    uint64_t id = 0;
    int fd = -1;
    bool connecting = false;
    bool eligible = false;  // may fill / be served from the cache
    bool is_head = false;
    std::string cache_key;
    std::string request_bytes;
    size_t sent = 0;
    Backend* backend = nullptr;
    std::vector<Backend*> tried;
    std::unique_ptr<ResponseParser> parser;
    double attempt_start = 0;
    double deadline = 0;
    bool last_timeout = false;
    std::vector<uint64_t> waiters;  // client ids; waiters[0] is the leader
  };

  // Accepting and client I/O.
  void on_accept(int listen_fd, bool admin);
  void on_client_event(uint64_t id, uint32_t events);
  void client_read(ClientConn& c);
  void process_requests(ClientConn& c);
  void handle_request(ClientConn& c, Request&& req);
  void handle_admin(ClientConn& c);
  void dispatch(ClientConn& c);
  void deliver(ClientConn& c, std::shared_ptr<const Response> resp, const char* x_cache, long age);
  void deliver_generated(ClientConn& c, int status, const std::string& body, bool keep_alive,
                         std::vector<Header> extra = {});
  void client_write(ClientConn& c);
  void finish_response(ClientConn& c);
  void close_client(ClientConn& c);
  ClientConn* find_client(uint64_t id);

  // Upstream fetches.
  void start_attempt(uint64_t fid);
  void on_fetch_event(uint64_t fid, uint32_t events);
  void attempt_failed(uint64_t fid, const char* reason, bool timeout);
  void complete_fetch(uint64_t fid);
  void fail_fetch(uint64_t fid, int status, const std::string& msg);
  void finish_fetch(std::unique_ptr<Fetch> f, std::shared_ptr<const Response> resp, bool share_with_all,
                    bool cacheable, const char* generated_label);
  void close_fetch_fd(Fetch& f);

  // Policy.
  bool cache_eligible(const Request& req) const;
  bool is_cacheable(const Response& r, double* ttl) const;
  void sweep_timeouts();
  void sweep_housekeeping();

  Config cfg_;
  EventLoop loop_;
  std::unique_ptr<Balancer> balancer_;
  std::unique_ptr<HealthChecker> health_;
  Cache cache_;
  RateLimiter limiter_;
  Metrics metrics_;

  int listen_fd_ = -1;
  int admin_fd_ = -1;
  int spare_fd_ = -1;  // reserved so we can shed connections on EMFILE
  uint16_t port_ = 0;
  uint16_t admin_port_ = 0;
  uint64_t next_id_ = 1;

  std::unordered_map<uint64_t, std::unique_ptr<ClientConn>> conns_;
  std::unordered_map<uint64_t, std::unique_ptr<Fetch>> fetches_;
  std::unordered_map<std::string, uint64_t> inflight_;    // cache key -> leader fetch id
  std::unordered_map<std::string, double> pass_until_;    // key -> time until which we skip coalescing
};

}  // namespace edge
