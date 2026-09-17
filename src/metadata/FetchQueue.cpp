#include "metadata/FetchQueue.h"

#include "core/Log.h"
#include "metadata/MetadataFetcher.h"

namespace mira::metadata {

void FetchQueue::Enqueue(const config::Config& config, api::EventBus& events, model::Game game, bool force) {
  if (!force && !config.GetBool("metadata.enabled")) return;

  queue_.Run([&config, &events, game = std::move(game)] {
    if (auto fetched = Fetch(config, game); !fetched) {
      log::Warn("metadata fetch failed for {}: {}", game.id, fetched.error().message);
      // The code as well as the message: a client deciding what to *do*
      // about a failure should not have to pattern-match English.
      events.Publish("game.metadata_failed", {{"id", game.id},
                                              {"code", fetched.error().code},
                                              {"error", fetched.error().message}});
    } else {
      events.Publish("game.metadata_ready", {{"id", game.id}});
    }
  });
}

}  // namespace mira::metadata
