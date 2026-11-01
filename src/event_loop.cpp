#include "event_loop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>

#include "util.h"

namespace edge {

namespace {
constexpr uint64_t kWakeTag = ~0ULL;
}

EventLoop::EventLoop() {
  epfd_ = epoll_create1(EPOLL_CLOEXEC);
  wakefd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.u64 = kWakeTag;
  epoll_ctl(epfd_, EPOLL_CTL_ADD, wakefd_, &ev);
}

EventLoop::~EventLoop() {
  if (wakefd_ >= 0) close(wakefd_);
  if (epfd_ >= 0) close(epfd_);
}

bool EventLoop::add(int fd, uint32_t events, Handler handler) {
  uint32_t gen = next_gen_++;
  epoll_event ev{};
  ev.events = events;
  ev.data.u64 = (static_cast<uint64_t>(gen) << 32) | static_cast<uint32_t>(fd);
  if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) return false;
  slots_[fd] = Slot{gen, std::make_shared<Handler>(std::move(handler))};
  return true;
}

bool EventLoop::modify(int fd, uint32_t events) {
  auto it = slots_.find(fd);
  if (it == slots_.end()) return false;
  epoll_event ev{};
  ev.events = events;
  ev.data.u64 = (static_cast<uint64_t>(it->second.gen) << 32) | static_cast<uint32_t>(fd);
  return epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

void EventLoop::remove(int fd) {
  auto it = slots_.find(fd);
  if (it == slots_.end()) return;
  epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
  slots_.erase(it);
}

void EventLoop::add_timer(double interval_seconds, std::function<void()> fn) {
  timers_.push_back({interval_seconds, now_seconds() + interval_seconds, std::move(fn)});
}

void EventLoop::defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }

void EventLoop::stop() {
  stop_.store(true);
  uint64_t one = 1;
  ssize_t r = write(wakefd_, &one, sizeof(one));
  (void)r;
}

int EventLoop::timeout_ms() const {
  if (timers_.empty()) return 1000;
  double next = timers_[0].next;
  for (const auto& t : timers_) next = std::min(next, t.next);
  double wait = next - now_seconds();
  if (wait <= 0) return 0;
  return static_cast<int>(std::ceil(wait * 1000.0));
}

void EventLoop::run_timers() {
  double now = now_seconds();
  for (size_t i = 0; i < timers_.size(); ++i) {
    if (now >= timers_[i].next) {
      timers_[i].next = now + timers_[i].interval;
      auto fn = timers_[i].fn;  // copy: fn may add timers
      fn();
    }
  }
}

void EventLoop::run_deferred() {
  while (!deferred_.empty()) {
    std::vector<std::function<void()>> batch;
    batch.swap(deferred_);
    for (auto& f : batch) f();
  }
}

void EventLoop::run() {
  constexpr int kMaxEvents = 256;
  epoll_event events[kMaxEvents];
  while (!stop_.load()) {
    int n = epoll_wait(epfd_, events, kMaxEvents, timeout_ms());
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < n; ++i) {
      uint64_t tag = events[i].data.u64;
      if (tag == kWakeTag) {
        uint64_t v;
        ssize_t r = read(wakefd_, &v, sizeof(v));
        (void)r;
        continue;
      }
      int fd = static_cast<int>(tag & 0xffffffffu);
      uint32_t gen = static_cast<uint32_t>(tag >> 32);
      auto it = slots_.find(fd);
      if (it == slots_.end() || it->second.gen != gen) continue;  // stale event
      std::shared_ptr<Handler> h = it->second.handler;  // keep alive during the call
      (*h)(events[i].events);
    }
    run_timers();
    run_deferred();
  }
  run_deferred();
}

}  // namespace edge
