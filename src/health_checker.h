#pragma once
// Active health checking. Runs in its own thread with blocking sockets and
// hard deadlines, so a slow or dead backend can never stall the event loop.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "balancer.h"

namespace edge {

struct HealthConfig {
  double interval = 1.0;  // seconds between probe rounds
  double timeout = 0.5;   // per-probe deadline (connect + response)
  std::string path = "/health";
  int fall = 2;  // consecutive failures before marking unhealthy
  int rise = 2;  // consecutive successes before marking healthy again
};

// One probe: true if the backend answered with status < 500 in time.
bool probe_backend(const Backend& b, const std::string& path, double timeout_seconds);

class HealthChecker {
 public:
  HealthChecker(Balancer& balancer, HealthConfig cfg);
  ~HealthChecker();
  void start();
  void stop();

 private:
  void run();
  Balancer& balancer_;
  HealthConfig cfg_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
};

}  // namespace edge
