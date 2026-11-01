#pragma once
// Backend pool, health state, and selection policies (round-robin and
// consistent hashing with virtual nodes).
#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace edge {

struct Backend {
  std::string name;  // "host:port"
  std::string host;
  uint16_t port = 0;
  sockaddr_in addr{};
  // Health state is shared between the event loop (passive checks) and the
  // health-checker thread (active checks), hence atomics.
  std::atomic<bool> healthy{true};
  std::atomic<int> consecutive_failures{0};
  std::atomic<int> consecutive_successes{0};
};

// Parses "host:port" and resolves host (IPv4). Returns nullptr and fills err on failure.
std::unique_ptr<Backend> make_backend(const std::string& spec, std::string* err);

class Balancer {
 public:
  enum class Policy { RoundRobin, ConsistentHash };

  Balancer(std::vector<std::unique_ptr<Backend>> backends, Policy policy, int vnodes = 128);

  // Picks a backend, preferring healthy ones and never returning anything in
  // `exclude`. If every candidate is unhealthy it fails open and returns an
  // unhealthy one rather than refusing all traffic. nullptr if all are excluded.
  Backend* pick(const std::string& key, const std::vector<Backend*>& exclude);

  // Passive health: called by the proxy on request outcomes.
  void report_failure(Backend* b, int fail_threshold);
  void report_success(Backend* b);

  // Active health: called by the health checker.
  void report_probe(Backend* b, bool ok, int fall, int rise);

  const std::vector<std::unique_ptr<Backend>>& backends() const { return backends_; }
  Policy policy() const { return policy_; }

  static uint64_t hash_key(const std::string& s);

 private:
  bool usable(Backend* b, const std::vector<Backend*>& exclude, bool require_healthy) const;

  std::vector<std::unique_ptr<Backend>> backends_;
  Policy policy_;
  std::vector<std::pair<uint64_t, int>> ring_;  // (hash, backend index), sorted
  uint64_t rr_counter_ = 0;
};

}  // namespace edge
