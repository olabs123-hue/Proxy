// Standalone fake origin for benchmarks: fake-backend --port 9001 --id a [--delay-ms N]
#include <signal.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "fake_backend.h"

int main(int argc, char** argv) {
  uint16_t port = 9001;
  std::string id = "a";
  int delay = 0;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--port")) port = static_cast<uint16_t>(atoi(argv[i + 1]));
    else if (!strcmp(argv[i], "--id")) id = argv[i + 1];
    else if (!strcmp(argv[i], "--delay-ms")) delay = atoi(argv[i + 1]);
  }
  signal(SIGPIPE, SIG_IGN);
  testutil::FakeBackend b(id, port, delay);
  if (!b.start()) {
    std::cerr << "fake-backend: cannot listen on " << port << "\n";
    return 1;
  }
  std::cerr << "fake-backend " << id << " on 127.0.0.1:" << b.port() << "\n";
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  int sig;
  sigwait(&set, &sig);
  return 0;
}
