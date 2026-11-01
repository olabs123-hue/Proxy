#pragma once
// Small shared helpers: clock, fd utilities, logging.
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace edge {

inline double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

inline bool set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline void set_nodelay(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

inline std::string ip_to_string(uint32_t ip_host_order) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip_host_order >> 24) & 255, (ip_host_order >> 16) & 255,
           (ip_host_order >> 8) & 255, ip_host_order & 255);
  return buf;
}

// Minimal logger. Level 0 = errors/startup only, 1 = verbose.
inline int& log_level() {
  static int level = 0;
  return level;
}
inline void log_msg(int level, const char* fmt, ...) {
  if (level > log_level()) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

}  // namespace edge
