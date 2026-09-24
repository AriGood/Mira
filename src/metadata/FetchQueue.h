#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/BackgroundQueue.h"
#include "model/Types.h"

namespace mira::metadata {

// Every metadata and cover fetch, run by a few workers rather than a thread
// each: a scan or a bulk refresh queues hundreds at once, and that many
// parallel requests get Steam and SteamGridDB to rate-limit all of them.
// Tracked games go ahead of store titles, which a store page queues by the
// hundred. A game already waiting isn't queued twice.
class FetchQueue {
public:
  ~FetchQueue();

  // A tracked game's full Fetch. `force` bypasses metadata.enabled — used
  // by the explicit refresh endpoint, where a user asking for a re-fetch
  // should work even with automatic fetching turned off. `announce`
  // publishes a "notification" event on failure — set for a user-initiated
  // fetch, left off for a background/bulk one so a fresh scan's fetches
  // don't each pop one. Success needs none: the cover changes.
  void Enqueue(const config::Config& config, api::EventBus& events, model::Game game, bool force = false,
               bool announce = false);

  // Store titles not installed yet, cover only (FetchCover). Each game is
  // synthetic: id "<source>-<ref>", the id it gets once installed. Publishes
  // library.artwork_ready/_failed ({source, ref}). Returns how many were
  // queued.
  int EnqueueTitles(const config::Config& config, api::EventBus& events, std::vector<model::Game> titles);

  // Blocks until nothing is queued or running. For tests.
  void WaitIdle();

private:
  struct Job {
    model::Game game;
    bool title = false;
    bool announce = false;
  };

  static constexpr int kWorkers = 3;

  // Starts workers up to kWorkers while there's work for them. Needs mutex_.
  void StartWorkers(const config::Config& config, api::EventBus& events);
  void Work(const config::Config& config, api::EventBus& events);
  static void Run(const config::Config& config, api::EventBus& events, const Job& job);

  std::mutex mutex_;
  std::condition_variable idle_;
  std::deque<Job> games_;   // tracked games, first
  std::deque<Job> titles_;  // store titles
  int workers_ = 0;
  int running_ = 0;
  std::atomic<bool> stopping_{false};
  BackgroundQueue threads_;  // last: joined before the rest is torn down
};

}  // namespace mira::metadata
