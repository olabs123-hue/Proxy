#include "metrics.h"

#include <cstdio>

namespace edge {

constexpr std::array<double, 13> Metrics::kBuckets;

void Metrics::observe_latency(double seconds) {
  ++latency_count_;
  latency_sum_ += seconds;
  for (size_t i = 0; i < kBuckets.size(); ++i) {
    if (seconds <= kBuckets[i]) {
      ++bucket_counts_[i];
      return;
    }
  }
  // Falls only into the implicit +Inf bucket.
}

void Metrics::count_response(int status) {
  int c = status / 100;
  ++responses_by_class[(c >= 1 && c <= 5) ? c : 0];
}

std::string Metrics::render(const Balancer& balancer, const Cache& cache, size_t active_connections,
                            size_t inflight_fetches) const {
  std::string out;
  char buf[256];
  auto counter = [&](const char* name, const char* help, uint64_t v) {
    snprintf(buf, sizeof(buf), "# HELP %s %s\n# TYPE %s counter\n%s %llu\n", name, help, name, name,
             static_cast<unsigned long long>(v));
    out += buf;
  };
  auto gauge = [&](const char* name, const char* help, double v) {
    snprintf(buf, sizeof(buf), "# HELP %s %s\n# TYPE %s gauge\n%s %g\n", name, help, name, name, v);
    out += buf;
  };
  counter("proxy_requests_total", "Requests received from clients.", requests_total);
  out += "# HELP proxy_responses_total Responses sent to clients by status class.\n# TYPE proxy_responses_total counter\n";
  for (int c = 1; c <= 5; ++c) {
    snprintf(buf, sizeof(buf), "proxy_responses_total{class=\"%dxx\"} %llu\n", c,
             static_cast<unsigned long long>(responses_by_class[c]));
    out += buf;
  }
  counter("proxy_cache_hits_total", "Requests served from cache.", cache_hits);
  counter("proxy_cache_misses_total", "Requests that went to an origin and may fill the cache.", cache_misses);
  counter("proxy_cache_coalesced_total", "Requests that waited on another in-flight origin fetch.", cache_coalesced);
  counter("proxy_cache_bypass_total", "Requests not eligible for caching.", cache_bypass);
  counter("proxy_rate_limited_total", "Requests rejected with 429.", rate_limited);
  counter("proxy_upstream_requests_total", "Upstream attempts started.", upstream_requests);
  counter("proxy_upstream_errors_total", "Upstream attempts that failed.", upstream_errors);
  counter("proxy_upstream_timeouts_total", "Upstream attempts that timed out.", upstream_timeouts);
  counter("proxy_upstream_retries_total", "Upstream attempts retried on another backend.", upstream_retries);
  counter("proxy_connections_accepted_total", "Client connections accepted.", connections_accepted);
  counter("proxy_connections_rejected_total", "Client connections refused (limit reached).", connections_rejected);
  counter("proxy_bad_requests_total", "Malformed or unsupported client requests.", bad_requests);
  gauge("proxy_active_connections", "Open client connections.", static_cast<double>(active_connections));
  gauge("proxy_inflight_upstream_fetches", "Origin fetches in progress.", static_cast<double>(inflight_fetches));
  gauge("proxy_cache_bytes", "Approximate bytes held in the cache.", static_cast<double>(cache.bytes()));
  gauge("proxy_cache_entries", "Objects held in the cache.", static_cast<double>(cache.entries()));
  counter("proxy_cache_evictions_total", "Objects evicted to stay under the size limit.", cache.evictions());

  out += "# HELP proxy_backend_up Whether a backend is considered healthy (1) or not (0).\n# TYPE proxy_backend_up gauge\n";
  for (const auto& b : balancer.backends()) {
    snprintf(buf, sizeof(buf), "proxy_backend_up{backend=\"%s\"} %d\n", b->name.c_str(), b->healthy.load() ? 1 : 0);
    out += buf;
  }

  out += "# HELP proxy_request_duration_seconds Time from request received to response fully written.\n";
  out += "# TYPE proxy_request_duration_seconds histogram\n";
  uint64_t cumulative = 0;
  for (size_t i = 0; i < kBuckets.size(); ++i) {
    cumulative += bucket_counts_[i];
    snprintf(buf, sizeof(buf), "proxy_request_duration_seconds_bucket{le=\"%g\"} %llu\n", kBuckets[i],
             static_cast<unsigned long long>(cumulative));
    out += buf;
  }
  snprintf(buf, sizeof(buf),
           "proxy_request_duration_seconds_bucket{le=\"+Inf\"} %llu\nproxy_request_duration_seconds_sum %g\n"
           "proxy_request_duration_seconds_count %llu\n",
           static_cast<unsigned long long>(latency_count_), latency_sum_, static_cast<unsigned long long>(latency_count_));
  out += buf;
  return out;
}

}  // namespace edge
