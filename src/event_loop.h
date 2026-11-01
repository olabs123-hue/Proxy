#pragma once
// A small single-threaded epoll reactor (level-triggered).
//
// Safety properties that matter for a proxy:
//  * Handlers may remove any fd (including their own) while events are being
//    dispatched. Each registration carries a generation number so a stale
//    event for a closed-and-reused fd number is never delivered to the new owner.
//  * Objects can be destroyed safely with defer(), which runs after the
//    current batch of events has been dispatched.
#include <sys/epoll.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace edge {

class EventLoop {
 public:
  using Handler = std::function<void(uint32_t events)>;

  EventLoop();
  ~EventLoop();
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  bool add(int fd, uint32_t events, Handler handler);
  bool modify(int fd, uint32_t events);
  void remove(int fd);  // does not close the fd

  void add_timer(double interval_seconds, std::function<void()> fn);
  void defer(std::function<void()> fn);

  void run();
  // Thread-safe and async-signal-safe (a single write to an eventfd).
  void stop();

 private:
  struct Slot {
    uint32_t gen;
    std::shared_ptr<Handler> handler;
  };
  struct Timer {
    double interval;
    double next;
    std::function<void()> fn;
  };
  void run_timers();
  void run_deferred();
  int timeout_ms() const;

  int epfd_ = -1;
  int wakefd_ = -1;
  uint32_t next_gen_ = 1;
  std::unordered_map<int, Slot> slots_;
  std::vector<Timer> timers_;
  std::vector<std::function<void()>> deferred_;
  std::atomic<bool> stop_{false};
};

}  // namespace edge
