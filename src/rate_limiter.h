#pragma once
// Per-client token-bucket rate limiter. The clock is injected (`now` argument)
// so behaviour is deterministic in tests.
#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace edge {

class RateLimiter {
 public:
  struct Decision {
    bool allowed;
    double retry_after;  // seconds until a token is available (0 if allowed)
  };

  // rate_per_sec <= 0 disables limiting.
  RateLimiter(double rate_per_sec, double burst, size_t max_entries = 100000)
      : rate_(rate_per_sec), burst_(std::max(burst, 1.0)), max_entries_(max_entries) {}

  bool enabled() const { return rate_ > 0; }

  Decision allow(uint32_t key, double now) {
    if (!enabled()) return {true, 0};
    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
      if (buckets_.size() >= max_entries_) {
        sweep(now);
        // Still full: evict an arbitrary bucket rather than grow without bound.
        if (buckets_.size() >= max_entries_) buckets_.erase(buckets_.begin());
      }
      it = buckets_.emplace(key, Bucket{burst_, now}).first;
    }
    Bucket& b = it->second;
    b.tokens = std::min(burst_, b.tokens + (now - b.last) * rate_);
    b.last = now;
    if (b.tokens >= 1.0) {
      b.tokens -= 1.0;
      return {true, 0};
    }
    return {false, (1.0 - b.tokens) / rate_};
  }

  // Drops buckets that have refilled completely (they carry no information).
  size_t sweep(double now) {
    size_t removed = 0;
    for (auto it = buckets_.begin(); it != buckets_.end();) {
      double tokens = it->second.tokens + (now - it->second.last) * rate_;
      if (tokens >= burst_) {
        it = buckets_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }

  size_t size() const { return buckets_.size(); }

 private:
  struct Bucket {
    double tokens;
    double last;
  };
  double rate_;
  double burst_;
  size_t max_entries_;
  std::unordered_map<uint32_t, Bucket> buckets_;
};

}  // namespace edge
