#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/BackgroundQueue.h"
#include "model/Types.h"

namespace mira::metadata {

// Cover art for store titles that aren't installed yet. A store page asks
// for every title at once, so these run one at a time rather than one
// thread each like FetchQueue. Publishes library.artwork_ready or
// library.artwork_failed ({source, ref}) per title.
class TitleArtQueue {
public:
  ~TitleArtQueue() { stopping_ = true; }

  // `titles` are synthetic games: id "<source>-<ref>", the id the game gets
  // once installed. Skips any already queued or running. Returns how many
  // were queued.
  int Enqueue(const config::Config& config, api::EventBus& events, std::vector<model::Game> titles);

private:
  void Drain(const config::Config& config, api::EventBus& events);

  std::mutex mutex_;
  std::deque<model::Game> pending_;
  std::set<std::string> queued_;  // ids in pending_ or running
  bool draining_ = false;
  std::atomic<bool> stopping_{false};
  BackgroundQueue worker_;  // last: joined before the rest is torn down
};

}  // namespace mira::metadata
