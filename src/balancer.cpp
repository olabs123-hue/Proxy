#include "balancer.h"

#include <arpa/inet.h>
#include <netdb.h>

#include <algorithm>
#include <cstring>

namespace edge {

std::unique_ptr<Backend> make_backend(const std::string& spec, std::string* err) {
  size_t colon = spec.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= spec.size()) {
    *err = "backend must look like host:port, got '" + spec + "'";
    return nullptr;
  }
  std::string host = spec.substr(0, colon);
  long port = std::strtol(spec.c_str() + colon + 1, nullptr, 10);
  if (port <= 0 || port > 65535) {
    *err = "invalid port in '" + spec + "'";
    return nullptr;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
  if (rc != 0 || !res) {
    *err = "cannot resolve '" + host + "': " + gai_strerror(rc);
    return nullptr;
  }
  auto b = std::make_unique<Backend>();
  b->name = host + ":" + std::to_string(port);
  b->host = host;
  b->port = static_cast<uint16_t>(port);
  b->addr = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
  b->addr.sin_port = htons(b->port);
  freeaddrinfo(res);
  return b;
}

uint64_t Balancer::hash_key(const std::string& s) {
  // FNV-1a followed by a murmur3-style finaliser for good bit dispersion.
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

Balancer::Balancer(std::vector<std::unique_ptr<Backend>> backends, Policy policy, int vnodes)
    : backends_(std::move(backends)), policy_(policy) {
  for (size_t i = 0; i < backends_.size(); ++i) {
    for (int v = 0; v < vnodes; ++v) {
      ring_.emplace_back(hash_key(backends_[i]->name + "#" + std::to_string(v)), static_cast<int>(i));
    }
  }
  std::sort(ring_.begin(), ring_.end());
}

bool Balancer::usable(Backend* b, const std::vector<Backend*>& exclude, bool require_healthy) const {
  if (std::find(exclude.begin(), exclude.end(), b) != exclude.end()) return false;
  return !require_healthy || b->healthy.load(std::memory_order_relaxed);
}

Backend* Balancer::pick(const std::string& key, const std::vector<Backend*>& exclude) {
  if (backends_.empty()) return nullptr;
  for (int pass = 0; pass < 2; ++pass) {
    bool require_healthy = (pass == 0);  // second pass = fail open
    if (policy_ == Policy::RoundRobin) {
      size_t n = backends_.size();
      uint64_t start = rr_counter_++;
      for (size_t i = 0; i < n; ++i) {
        Backend* b = backends_[(start + i) % n].get();
        if (usable(b, exclude, require_healthy)) return b;
      }
    } else {
      uint64_t h = hash_key(key);
      auto it = std::lower_bound(ring_.begin(), ring_.end(), std::make_pair(h, -1));
      for (size_t i = 0; i < ring_.size(); ++i) {
        if (it == ring_.end()) it = ring_.begin();
        Backend* b = backends_[it->second].get();
        if (usable(b, exclude, require_healthy)) return b;
        ++it;
      }
    }
  }
  return nullptr;
}

void Balancer::report_failure(Backend* b, int fail_threshold) {
  b->consecutive_successes.store(0);
  int f = b->consecutive_failures.fetch_add(1) + 1;
  if (f >= fail_threshold) b->healthy.store(false);
}

void Balancer::report_success(Backend* b) { b->consecutive_failures.store(0); }

void Balancer::report_probe(Backend* b, bool ok, int fall, int rise) {
  if (ok) {
    b->consecutive_failures.store(0);
    int s = b->consecutive_successes.fetch_add(1) + 1;
    if (s >= rise) b->healthy.store(true);
  } else {
    b->consecutive_successes.store(0);
    int f = b->consecutive_failures.fetch_add(1) + 1;
    if (f >= fall) b->healthy.store(false);
  }
}

}  // namespace edge
