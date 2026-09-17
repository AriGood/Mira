#pragma once

#include <functional>
#include <future>
#include <mutex>
#include <vector>

namespace mira {

// Owns every task run through it, joining each one no later than its own
// destruction — same "own it, don't detach it" discipline as
// proc::ProcessSupervisor's watcher threads. A detached thread capturing
// references to caller state is unsafe once that state can be torn down
// before the thread finishes, which TSan caught in a test using this class.
class BackgroundQueue {
public:
  ~BackgroundQueue();
  BackgroundQueue() = default;
  BackgroundQueue(const BackgroundQueue&) = delete;
  BackgroundQueue& operator=(const BackgroundQueue&) = delete;

  // Fire-and-forget from the caller's point of view; tracked and joined by
  // this queue. Safe to call from multiple threads concurrently.
  void Run(std::function<void()> task);

private:
  std::mutex mutex_;
  std::vector<std::future<void>> pending_;
};

}  // namespace mira
