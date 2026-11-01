#pragma once
// A tiny threaded HTTP/1.1 origin used by the integration tests and the
// benchmarks. Header-only. Not part of the proxy itself.
//
// Paths:
//   /health            200 "ok" (503 while set_healthy(false)); not counted as a hit
//   /_stats            "hits=N"
//   /slow, anything    sleeps delay_ms, replies "backend:<id> path:<path>"
//   /nocache           same, with Cache-Control: no-store
//   /ttl?s=N           Cache-Control: max-age=N
//   /cookie            sets a cookie (must not be cached)
//   /chunked           chunked transfer-encoding body "hello world"
//   /close             close-delimited body (no Content-Length)
//   /big?n=BYTES       a body of BYTES 'x'
//   /status/NNN        replies with status NNN
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace testutil {

class FakeBackend {
 public:
  explicit FakeBackend(std::string id, uint16_t port = 0, int delay_ms = 0)
      : id_(std::move(id)), port_(port), delay_ms_(delay_ms) {}
  ~FakeBackend() { stop(); }

  bool start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port_);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || listen(listen_fd_, 1024) != 0) {
      close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    socklen_t len = sizeof(sa);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&sa), &len);
    port_ = ntohs(sa.sin_port);
    running_ = true;
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
  }

  // Abruptly stops serving and resets open connections, like a crashed process.
  void stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);  // wakes the blocked accept()
    if (accept_thread_.joinable()) accept_thread_.join();
    if (listen_fd_ >= 0) {  // only after the accept thread can no longer read it
      close(listen_fd_);
      listen_fd_ = -1;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (int fd : conns_) {
        linger l{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        shutdown(fd, SHUT_RDWR);
      }
    }
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return active_ == 0; });
  }

  uint16_t port() const { return port_; }
  std::string address() const { return "127.0.0.1:" + std::to_string(port_); }
  uint64_t hits() const { return hits_.load(); }
  uint64_t hits_for(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = path_hits_.find(path);
    return it == path_hits_.end() ? 0 : it->second;
  }
  void set_delay_ms(int ms) { delay_ms_ = ms; }
  void set_healthy(bool h) { healthy_ = h; }

 private:
  void accept_loop() {
    while (running_) {
      int fd = accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        if (!running_) break;
        continue;
      }
      {
        std::lock_guard<std::mutex> lk(mu_);
        conns_.insert(fd);
        ++active_;
      }
      std::thread([this, fd] {
        serve(fd);
        std::lock_guard<std::mutex> lk(mu_);
        conns_.erase(fd);
        close(fd);
        --active_;
        cv_.notify_all();
      }).detach();
    }
  }

  static bool write_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
      ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
      if (n <= 0) return false;
      off += static_cast<size_t>(n);
    }
    return true;
  }

  static std::string reply(int status, const char* reason, const std::string& body, const std::string& extra = "",
                           bool close_conn = false, bool with_length = true) {
    std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    if (with_length) r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += extra;
    r += close_conn ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
    return r + body;
  }

  void serve(int fd) {
    std::string buf;
    char tmp[4096];
    while (running_) {
      size_t end;
      while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return;
        buf.append(tmp, static_cast<size_t>(n));
      }
      std::string head = buf.substr(0, end);
      buf.erase(0, end + 4);
      size_t sp1 = head.find(' ');
      size_t sp2 = head.find(' ', sp1 + 1);
      if (sp1 == std::string::npos || sp2 == std::string::npos) return;
      std::string method = head.substr(0, sp1);
      std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
      std::string lower_head = head;
      for (auto& ch : lower_head) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
      bool close_conn = lower_head.find("connection: close") != std::string::npos;
      std::string path = target.substr(0, target.find('?'));
      std::string query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);

      std::string out;
      bool force_close = false;
      if (path == "/health") {
        out = healthy_ ? reply(200, "OK", "ok\n", "", close_conn) : reply(503, "Service Unavailable", "unhealthy\n", "", close_conn);
      } else if (path == "/_stats") {
        out = reply(200, "OK", "hits=" + std::to_string(hits_.load()) + "\n", "", close_conn);
      } else {
        ++hits_;
        {
          std::lock_guard<std::mutex> lk(mu_);
          ++path_hits_[path];
        }
        int d = delay_ms_;
        if (d > 0) std::this_thread::sleep_for(std::chrono::milliseconds(d));
        std::string body = "backend:" + id_ + " path:" + path + "\n";
        std::string extra = "X-Backend: " + id_ + "\r\n";
        if (path == "/nocache") {
          out = reply(200, "OK", body, extra + "Cache-Control: no-store\r\n", close_conn);
        } else if (path == "/ttl") {
          out = reply(200, "OK", body, extra + "Cache-Control: max-age=" + query.substr(query.find('=') == std::string::npos ? 0 : query.find('=') + 1) + "\r\n", close_conn);
        } else if (path == "/cookie") {
          out = reply(200, "OK", body, extra + "Set-Cookie: sid=abc\r\n", close_conn);
        } else if (path == "/chunked") {
          out = "HTTP/1.1 200 OK\r\n" + extra + "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
                "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
          force_close = true;
        } else if (path == "/close") {
          out = "HTTP/1.1 200 OK\r\n" + extra + "Connection: close\r\n\r\n" + body;
          force_close = true;
        } else if (path == "/big") {
          size_t n = static_cast<size_t>(std::strtoull(query.substr(query.find('=') == std::string::npos ? 0 : query.find('=') + 1).c_str(), nullptr, 10));
          out = reply(200, "OK", std::string(n, 'x'), extra, close_conn);
        } else if (path.rfind("/status/", 0) == 0) {
          int code = std::atoi(path.c_str() + 8);
          out = reply(code, "Status", body, extra, close_conn);
        } else {
          out = reply(200, "OK", body, extra, close_conn);
        }
        if (method == "HEAD") {
          size_t he = out.find("\r\n\r\n");
          if (he != std::string::npos) out.resize(he + 4);
        }
      }
      if (!write_all(fd, out)) return;
      if (close_conn || force_close) return;
    }
  }

  std::string id_;
  uint16_t port_;
  std::atomic<int> delay_ms_;
  std::atomic<bool> healthy_{true};
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> hits_{0};
  int listen_fd_ = -1;
  std::thread accept_thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::set<int> conns_;
  int active_ = 0;
  std::map<std::string, uint64_t> path_hits_;
};

}  // namespace testutil
