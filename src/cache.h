#pragma once
// Byte-bounded LRU response cache with per-entry TTL. Not thread-safe: it is
// owned by the single event-loop thread. The clock is injected for testing.
#include <list>
#include <memory>
#include <string>
#include <unordered_map>

#include "http.h"

namespace edge {

class Cache {
 public:
  explicit Cache(size_t max_bytes, size_t max_object_bytes = 4 * 1024 * 1024)
      : max_bytes_(max_bytes), max_object_(max_object_bytes) {}

  // Returns nullptr on miss or expiry. A hit becomes most-recently-used.
  std::shared_ptr<const Response> get(const std::string& key, double now);

  // Stores a copy of `resp`. Returns false if it is too large to cache.
  bool put(const std::string& key, std::shared_ptr<const Response> resp, double ttl_seconds, double now);

  void erase(const std::string& key);
  size_t purge_expired(double now);

  size_t bytes() const { return bytes_; }
  size_t entries() const { return map_.size(); }
  size_t evictions() const { return evictions_; }

 private:
  struct Entry {
    std::string key;
    std::shared_ptr<const Response> resp;
    double expires_at;
    size_t size;
  };
  using List = std::list<Entry>;
  static size_t entry_size(const std::string& key, const Response& r);
  void remove(List::iterator it);

  size_t max_bytes_;
  size_t max_object_;
  size_t bytes_ = 0;
  size_t evictions_ = 0;
  List lru_;  // front = most recently used
  std::unordered_map<std::string, List::iterator> map_;
};

}  // namespace edge
