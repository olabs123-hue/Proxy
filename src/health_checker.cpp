#include "health_checker.h"

#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "util.h"

namespace edge {

namespace {
// Waits for `events` on fd until deadline. Returns true if ready.
bool wait_fd(int fd, short events, double deadline) {
  for (;;) {
    double remaining = deadline - now_seconds();
    if (remaining <= 0) return false;
    pollfd p{fd, events, 0};
    int rc = poll(&p, 1, static_cast<int>(remaining * 1000) + 1);
    if (rc > 0) return true;
    if (rc < 0 && errno != EINTR) return false;
  }
}
}  // namespace

bool probe_backend(const Backend& b, const std::string& path, double timeout_seconds) {
  double deadline = now_seconds() + timeout_seconds;
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  struct Closer {
    int fd;
    ~Closer() { close(fd); }
  } closer{fd};
  set_nonblocking(fd);
  int rc = connect(fd, reinterpret_cast<const sockaddr*>(&b.addr), sizeof(b.addr));
  if (rc < 0 && errno != EINPROGRESS) return false;
  if (rc < 0) {
    if (!wait_fd(fd, POLLOUT, deadline)) return false;
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) return false;
  }
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + b.name + "\r\nUser-Agent: edge-proxy-health\r\nConnection: close\r\n\r\n";
  size_t sent = 0;
  while (sent < req.size()) {
    ssize_t n = send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (!wait_fd(fd, POLLOUT, deadline)) return false;
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  // Read just enough to see the status line.
  std::string buf;
  char tmp[512];
  while (buf.find("\r\n") == std::string::npos && buf.size() < 4096) {
    if (!wait_fd(fd, POLLIN, deadline)) return false;
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n > 0) buf.append(tmp, static_cast<size_t>(n));
    else if (n == 0) break;
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
  }
  if (buf.size() < 12 || buf.compare(0, 5, "HTTP/") != 0) return false;
  int status = std::atoi(buf.c_str() + 9);
  return status >= 200 && status < 500;
}

HealthChecker::HealthChecker(Balancer& balancer, HealthConfig cfg) : balancer_(balancer), cfg_(std::move(cfg)) {}

HealthChecker::~HealthChecker() { stop(); }

void HealthChecker::start() {
  if (thread_.joinable()) return;
  thread_ = std::thread([this] { run(); });
}

void HealthChecker::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void HealthChecker::run() {
  std::unique_lock<std::mutex> lk(mu_);
  while (!stopping_) {
    lk.unlock();
    for (const auto& b : balancer_.backends()) {
      bool ok = probe_backend(*b, cfg_.path, cfg_.timeout);
      bool was_healthy = b->healthy.load();
      balancer_.report_probe(b.get(), ok, cfg_.fall, cfg_.rise);
      bool now_healthy = b->healthy.load();
      if (was_healthy != now_healthy)
        log_msg(0, "health: backend %s is now %s", b->name.c_str(), now_healthy ? "UP" : "DOWN");
    }
    lk.lock();
    cv_.wait_for(lk, std::chrono::duration<double>(cfg_.interval), [this] { return stopping_; });
  }
}

}  // namespace edge
