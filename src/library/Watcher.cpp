#include "library/Watcher.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/Log.h"
#include "library/Scanner.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

constexpr int kWatchMask = IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE;

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Sum of file sizes under `dir`, used only to tell whether a directory is
// still being written to. Errors on individual entries (permission, a file
// vanishing mid-walk) are skipped rather than failing the whole check — this
// only has to be approximately right, not exact.
std::uintmax_t TotalSize(const fs::path& dir) {
  std::uintmax_t total = 0;
  std::error_code ec;
  for (const auto& entry : fs::recursive_directory_iterator(
           dir, fs::directory_options::skip_permission_denied, ec)) {
    std::error_code size_ec;
    if (entry.is_regular_file(size_ec)) total += entry.file_size(size_ec);
  }
  return total;
}

}  // namespace

Watcher::Watcher(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {
  // Created here, before Run() ever starts on another thread, specifically
  // because Stop() (called from whatever thread owns the Watcher object) must
  // never race Run()'s own setup — thread creation is a happens-before point
  // for anything written beforehand, so this is the one fd that cannot be
  // deferred to Run() like the other three. TSan caught this exact race
  // during development (see tests/watcher_test.cpp).
  stop_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (stop_fd_ < 0) log::Error("Watcher: failed to create stop eventfd: {}", std::strerror(errno));
}

Watcher::~Watcher() {
  if (inotify_fd_ >= 0) ::close(inotify_fd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
  if (timer_fd_ >= 0) ::close(timer_fd_);
  if (stop_fd_ >= 0) ::close(stop_fd_);
}

void Watcher::Stop() {
  if (stop_fd_ < 0) return;
  const uint64_t one = 1;
  if (::write(stop_fd_, &one, sizeof(one)) < 0) {
    log::Error("Watcher::Stop: failed to signal eventfd: {}", std::strerror(errno));
  }
}

void Watcher::RearmTimer() {
  itimerspec spec{};
  if (pending_.empty()) {
    timerfd_settime(timer_fd_, 0, &spec, nullptr);  // all-zero disarms it
    return;
  }
  // A fixed short poll interval while anything is pending — not an idle
  // wakeup (see the class comment): it only runs while a directory is
  // actively being watched for size stability, and disarms the instant
  // pending_ empties.
  constexpr long kPollMs = 500;
  spec.it_value.tv_sec = kPollMs / 1000;
  spec.it_value.tv_nsec = (kPollMs % 1000) * 1000000L;
  timerfd_settime(timer_fd_, 0, &spec, nullptr);
}

void Watcher::ScheduleCheck(const fs::path& root, const fs::path& dir) {
  Pending entry;
  entry.root = root;
  entry.last_size = TotalSize(dir);
  entry.stable_since_ms = NowMs();
  pending_[dir.string()] = entry;
  RearmTimer();
}

void Watcher::Run() {
  inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (inotify_fd_ < 0 || epoll_fd_ < 0 || timer_fd_ < 0 || stop_fd_ < 0) {
    log::Error("Watcher: failed to set up inotify/epoll: {}", std::strerror(errno));
    return;
  }

  for (const fs::path& root : config_.GetPathArray("library_roots")) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
      log::Warn("Watcher: library root {} does not exist yet — not watched until it does "
               "(restart mirad once it's created)",
               root.string());
      continue;
    }
    const int wd = inotify_add_watch(inotify_fd_, root.c_str(), kWatchMask);
    if (wd < 0) {
      log::Error("Watcher: could not watch {}: {}", root.string(), std::strerror(errno));
      continue;
    }
    watch_to_root_[wd] = root;
    log::Info("watching {}", root.string());
  }

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = inotify_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, inotify_fd_, &ev);
  ev.data.fd = timer_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev);
  ev.data.fd = stop_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, stop_fd_, &ev);

  epoll_event fired[8];
  while (true) {
    const int n = epoll_wait(epoll_fd_, fired, 8, -1);  // blocks; no timeout at rest
    if (n < 0) {
      if (errno == EINTR) continue;
      log::Error("Watcher: epoll_wait failed: {}", std::strerror(errno));
      break;
    }
    bool stop_requested = false;
    for (int i = 0; i < n; ++i) {
      if (fired[i].data.fd == stop_fd_) {
        stop_requested = true;
      } else if (fired[i].data.fd == inotify_fd_) {
        HandleInotify();
      } else if (fired[i].data.fd == timer_fd_) {
        uint64_t expirations = 0;
        [[maybe_unused]] auto _ = ::read(timer_fd_, &expirations, sizeof(expirations));
        HandleDebounceTick();
      }
    }
    if (stop_requested) break;
  }
}

void Watcher::HandleInotify() {
  // Sized for several events with reasonably long filenames; inotify_event
  // is variable-length (name follows the struct), so this is read in a loop
  // rather than assumed to be one event per read.
  alignas(inotify_event) char buffer[4096];
  library::Scanner scanner(config_, games_, events_);

  while (true) {
    const ssize_t n = ::read(inotify_fd_, buffer, sizeof(buffer));
    if (n <= 0) break;  // EAGAIN (nothing more queued) or an error either way

    size_t offset = 0;
    while (offset < static_cast<size_t>(n)) {
      const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
      offset += sizeof(inotify_event) + event->len;

      const auto root_it = watch_to_root_.find(event->wd);
      if (root_it == watch_to_root_.end() || event->len == 0) continue;
      const fs::path path = root_it->second / event->name;

      if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
        std::error_code ec;
        if (fs::is_directory(path, ec)) ScheduleCheck(root_it->second, path);
      } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
        pending_.erase(path.string());  // no point finishing a debounce for a path that's gone
        // A deletion needs no debounce — rescan this root now so a removed
        // game is marked missing promptly.
        scanner.ScanRoot(root_it->second);
      }
    }
  }
  RearmTimer();
}

void Watcher::HandleDebounceTick() {
  const std::int64_t now = NowMs();
  const std::int64_t debounce_ms = config_.GetInt("scan.debounce_ms");
  std::vector<std::string> settled;

  for (auto& [path, entry] : pending_) {
    const std::uintmax_t current_size = TotalSize(path);
    if (current_size != entry.last_size) {
      entry.last_size = current_size;
      entry.stable_since_ms = now;
      continue;
    }
    if (now - entry.stable_since_ms >= debounce_ms) settled.push_back(path);
  }

  if (!settled.empty()) {
    library::Scanner scanner(config_, games_, events_);
    for (const std::string& path : settled) {
      const fs::path root = pending_.at(path).root;
      pending_.erase(path);
      log::Info("{} settled, scanning {}", path, root.string());
      scanner.ScanRoot(root);
    }
  }
  RearmTimer();
}

}  // namespace mira::library
