#pragma once
// Counters and a latency histogram, rendered in Prometheus text format.
// Updated only from the event-loop thread.
#include <array>
#include <cstdint>
#include <string>

#include "balancer.h"
#include "cache.h"

namespace edge {

class Metrics {
 public:
  static constexpr std::array<double, 13> kBuckets = {0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05,
                                                      0.1,    0.25,  0.5,    1.0,   2.5,  5.0};

  void observe_latency(double seconds);
  void count_response(int status);

  uint64_t requests_total = 0;
  uint64_t responses_by_class[6] = {0};  // index = status / 100 (1..5), 0 = other
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t cache_coalesced = 0;
  uint64_t cache_bypass = 0;
  uint64_t rate_limited = 0;
  uint64_t upstream_errors = 0;
  uint64_t upstream_timeouts = 0;
  uint64_t upstream_retries = 0;
  uint64_t upstream_requests = 0;
  uint64_t connections_accepted = 0;
  uint64_t connections_rejected = 0;
  uint64_t bad_requests = 0;

  std::string render(const Balancer& balancer, const Cache& cache, size_t active_connections,
                     size_t inflight_fetches) const;

 private:
  uint64_t bucket_counts_[kBuckets.size()] = {0};  // non-cumulative
  uint64_t latency_count_ = 0;
  double latency_sum_ = 0;
};

}  // namespace edge
