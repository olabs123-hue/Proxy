#include "cache.h"

namespace edge {

size_t Cache::entry_size(const std::string& key, const Response& r) {
  size_t n = key.size() + r.body.size() + r.reason.size() + 128;
  for (const auto& h : r.headers) n += h.name.size() + h.value.size() + 4;
  return n;
}

void Cache::remove(List::iterator it) {
  bytes_ -= it->size;
  map_.erase(it->key);
  lru_.erase(it);
}

std::shared_ptr<const Response> Cache::get(const std::string& key, double now) {
  auto m = map_.find(key);
  if (m == map_.end()) return nullptr;
  auto it = m->second;
  if (now >= it->expires_at) {
    remove(it);
    return nullptr;
  }
  lru_.splice(lru_.begin(), lru_, it);  // iterator stays valid
  return it->resp;
}

bool Cache::put(const std::string& key, std::shared_ptr<const Response> resp, double ttl_seconds, double now) {
  if (!resp || ttl_seconds <= 0) return false;
  size_t size = entry_size(key, *resp);
  if (size > max_object_ || size > max_bytes_) return false;
  auto m = map_.find(key);
  if (m != map_.end()) remove(m->second);
  lru_.push_front(Entry{key, std::move(resp), now + ttl_seconds, size});
  map_[key] = lru_.begin();
  bytes_ += size;
  while (bytes_ > max_bytes_ && !lru_.empty()) {
    remove(std::prev(lru_.end()));
    ++evictions_;
  }
  return true;
}

void Cache::erase(const std::string& key) {
  auto m = map_.find(key);
  if (m != map_.end()) remove(m->second);
}

size_t Cache::purge_expired(double now) {
  size_t removed = 0;
  for (auto it = lru_.begin(); it != lru_.end();) {
    if (now >= it->expires_at) {
      auto next = std::next(it);
      remove(it);
      it = next;
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

}  // namespace edge
