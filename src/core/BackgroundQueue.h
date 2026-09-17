#pragma once

#include <functional>
#include <future>
#include <mutex>
#include <vector>

namespace mira {

// Owns every task run through it, joining each one (via a std::future's
// blocking destructor) no later than its own destruction — the "own it,
// don't detach it" discipline proc::ProcessSupervisor already uses for its
// per-game watcher threads, generalized. A bare detached std::thread
// capturing references to its caller's state is only safe when those
// references are guaranteed to outlive the thread; that's true in
// production (Server/Watcher's own daemon-lifetime members) but not in a
// test that constructs short-lived dependencies and tears them down right
// after the call returns — TSan caught exactly this once (see
// metadata::FetchQueue, the first user of this class).
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
