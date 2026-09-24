#include "metadata/TitleArtQueue.h"

#include "core/Log.h"
#include "metadata/MetadataFetcher.h"

namespace mira::metadata {

int TitleArtQueue::Enqueue(const config::Config& config, api::EventBus& events, std::vector<model::Game> titles) {
  std::lock_guard lock(mutex_);
  int queued = 0;
  for (model::Game& title : titles) {
    if (!queued_.insert(title.id).second) continue;
    pending_.push_back(std::move(title));
    ++queued;
  }
  if (!pending_.empty() && !draining_) {
    draining_ = true;
    worker_.Run([this, &config, &events] { Drain(config, events); });
  }
  return queued;
}

void TitleArtQueue::Drain(const config::Config& config, api::EventBus& events) {
  for (;;) {
    model::Game title;
    {
      // Cleared under the same lock as the empty check, or an Enqueue in
      // between would see a worker that's about to exit.
      std::lock_guard lock(mutex_);
      if (pending_.empty() || stopping_) {
        draining_ = false;
        return;
      }
      title = std::move(pending_.front());
      pending_.pop_front();
    }
    const Result<void> fetched = FetchCover(config, title);
    if (!fetched) {
      log::Debug("no cover for {} {}: {}", title.source, title.source_ref, fetched.error().message);
      events.Publish("library.artwork_failed", {{"source", title.source},
                                                {"ref", title.source_ref},
                                                {"code", fetched.error().code}});
    } else {
      events.Publish("library.artwork_ready", {{"source", title.source}, {"ref", title.source_ref}});
    }
    // Forgotten once done: a failure can be retried, and a success is
    // cached, which the caller checks before queuing.
    std::lock_guard lock(mutex_);
    queued_.erase(title.id);
  }
}

}  // namespace mira::metadata
