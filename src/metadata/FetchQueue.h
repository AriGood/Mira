#pragma once

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/BackgroundQueue.h"
#include "model/Types.h"

namespace mira::metadata {

// Thin wrapper over BackgroundQueue that builds the actual fetch closure —
// see BackgroundQueue's class comment for why background work is owned and
// joined here rather than detached.
class FetchQueue {
public:
  // Fire-and-forget from the caller's point of view; internally tracked and
  // joined by the time this FetchQueue is destroyed. `force` bypasses
  // metadata.enabled — used by the explicit refresh endpoint, where a user
  // asking for a re-fetch should work even with automatic fetching turned
  // off. `announce` publishes a "notification" event on failure — set for a
  // user-initiated fetch, left off for a background/bulk one so a fresh
  // scan's fetches don't each pop one. Success needs none: the cover changes.
  void Enqueue(const config::Config& config, api::EventBus& events, model::Game game, bool force = false,
               bool announce = false);

private:
  BackgroundQueue queue_;
};

}  // namespace mira::metadata
