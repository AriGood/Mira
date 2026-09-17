#pragma once

#include <filesystem>
#include <map>

#include "api/EventBus.h"
#include "config/Config.h"
#include "store/GameStore.h"

namespace mira::library {

// Watches every enabled library root for new game folders and rescans
// automatically — this is what makes "drop a folder in and it's picked up"
// true without running `mira scan` by hand.
//
// One inotify watch per root, non-recursive (so watch count stays O(number
// of roots), never O(library size) — see docs/architecture.md on why
// recursive watching is the usual way this kind of daemon gets expensive).
// A brand-new directory is not scanned the instant it appears: it might
// still be mid-copy, so it enters a debounce set and is only scanned once
// its total size has been unchanged for `scan.debounce_ms`. The whole loop
// blocks in epoll_wait with no timeout while nothing is pending — the only
// time it wakes on a timer is while a directory is actively being watched
// for size stability, never at rest (see EventBus::WaitNext for the same
// pattern applied to SSE connections).
//
// Runs on its own thread (Run() blocks); Stop() is safe to call from any
// other thread and unblocks it promptly via an eventfd rather than a signal
// or a polled flag.
class Watcher {
public:
  Watcher(config::Config& config, store::GameStore& games, api::EventBus& events);
  ~Watcher();
  Watcher(const Watcher&) = delete;
  Watcher& operator=(const Watcher&) = delete;

  // Reads library_roots once at construction time; adding or removing a root
  // afterwards needs a restart to take effect (no live re-registration yet).
  void Run();
  void Stop();

private:
  struct Pending {
    std::filesystem::path root;
    std::uintmax_t last_size = 0;
    std::int64_t stable_since_ms = 0;
    bool is_archive = false;  // extract-and-remove on settle, instead of scanning it as a folder
  };

  void HandleInotify();
  void HandleDebounceTick();
  void ScheduleCheck(const std::filesystem::path& root, const std::filesystem::path& path, bool is_archive);
  void RearmTimer();

  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;

  int inotify_fd_ = -1;
  int epoll_fd_ = -1;
  int timer_fd_ = -1;
  int stop_fd_ = -1;

  std::map<int, std::filesystem::path> watch_to_root_;  // inotify watch descriptor -> root
  std::map<std::string, Pending> pending_;               // absolute dir path -> debounce state
};

}  // namespace mira::library
