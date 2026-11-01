// edge-proxy: command-line entry point.
#include <signal.h>
#include <sys/resource.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "proxy.h"
#include "util.h"

namespace {
edge::Proxy* g_proxy = nullptr;
void on_signal(int) {
  if (g_proxy) g_proxy->stop();  // a single write() to an eventfd: async-signal-safe
}

void usage() {
  std::cerr << R"(edge-proxy: a small HTTP/1.1 reverse proxy (epoll, C++17)

Usage: edge-proxy --backend host:port [--backend host:port ...] [options]

  --bind ADDR            listen address (default 127.0.0.1)
  --port N               listen port (default 8080)
  --admin-port N         /metrics and /healthz port on 127.0.0.1; -1 disables (default 9100)
  --policy rr|hash       round-robin (default) or consistent hashing on host+path
  --rate N               per-client-IP requests/second limit (default: off)
  --burst N              token-bucket burst size (default: 2 x rate)
  --no-cache             disable the response cache and request coalescing
  --cache-mb N           cache size in MiB (default 64)
  --cache-ttl SECONDS    default TTL when the origin gives no max-age (default 30)
  --connect-timeout S    upstream connect timeout (default 2)
  --upstream-timeout S   per-attempt upstream timeout (default 10)
  --max-conns N          maximum client connections (default 10000)
  --health-path PATH     active health-check path (default /health)
  --health-interval S    seconds between health probes (default 1)
  --verbose              log upstream failures
  --help
)";
}
}  // namespace

int main(int argc, char** argv) {
  edge::Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--backend") cfg.backends.push_back(need("--backend"));
    else if (a == "--bind") cfg.bind_addr = need("--bind");
    else if (a == "--port") cfg.listen_port = static_cast<uint16_t>(std::atoi(need("--port")));
    else if (a == "--admin-port") cfg.admin_port = std::atoi(need("--admin-port"));
    else if (a == "--policy") {
      std::string p = need("--policy");
      if (p == "rr") cfg.policy = edge::Balancer::Policy::RoundRobin;
      else if (p == "hash") cfg.policy = edge::Balancer::Policy::ConsistentHash;
      else { std::cerr << "unknown policy '" << p << "'\n"; return 2; }
    }
    else if (a == "--rate") cfg.rate_per_sec = std::atof(need("--rate"));
    else if (a == "--burst") cfg.burst = std::atof(need("--burst"));
    else if (a == "--no-cache") cfg.cache_enabled = false;
    else if (a == "--cache-mb") cfg.cache_bytes = static_cast<size_t>(std::atof(need("--cache-mb")) * 1024 * 1024);
    else if (a == "--cache-ttl") cfg.cache_ttl = std::atof(need("--cache-ttl"));
    else if (a == "--connect-timeout") cfg.connect_timeout = std::atof(need("--connect-timeout"));
    else if (a == "--upstream-timeout") cfg.upstream_timeout = std::atof(need("--upstream-timeout"));
    else if (a == "--max-conns") cfg.max_conns = static_cast<size_t>(std::atol(need("--max-conns")));
    else if (a == "--health-path") cfg.health.path = need("--health-path");
    else if (a == "--health-interval") cfg.health.interval = std::atof(need("--health-interval"));
    else if (a == "--verbose") edge::log_level() = 1;
    else if (a == "--help" || a == "-h") { usage(); return 0; }
    else { std::cerr << "unknown option '" << a << "'\n"; usage(); return 2; }
  }

  // Many connections need many descriptors.
  rlimit rl{};
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }

  edge::Proxy proxy(cfg);
  std::string err;
  if (!proxy.init(&err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  g_proxy = &proxy;
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  std::cerr << "edge-proxy listening on " << cfg.bind_addr << ":" << proxy.port();
  if (cfg.admin_port >= 0) std::cerr << ", metrics on 127.0.0.1:" << proxy.admin_port() << "/metrics";
  std::cerr << " (" << cfg.backends.size() << " backends)\n";
  proxy.run();
  std::cerr << "edge-proxy: shut down\n";
  return 0;
}
