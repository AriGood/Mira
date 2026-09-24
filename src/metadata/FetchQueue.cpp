#include "metadata/FetchQueue.h"

#include <format>

#include "core/Log.h"
#include "metadata/MetadataFetcher.h"

namespace mira::metadata {

void FetchQueue::Enqueue(const config::Config& config, api::EventBus& events, model::Game game, bool force,
                         bool announce) {
  if (!force && !config.GetBool("metadata.enabled")) return;

  queue_.Run([&config, &events, game = std::move(game), announce] {
    if (auto fetched = Fetch(config, game); !fetched) {
      log::Warn("metadata fetch failed for {}: {}", game.id, fetched.error().message);
      // The code as well as the message: a client deciding what to *do*
      // about a failure should not have to pattern-match English.
      events.Publish("game.metadata_failed", {{"id", game.id},
                                              {"code", fetched.error().code},
                                              {"error", fetched.error().message}});
      if (announce && fetched.error().code != "no_steamgriddb_key") {
        events.PublishNotification(
            model::NotifyLevel::Warning,
            std::format("No metadata found for \"{}\": {}", game.id,
                       fetched.error().message.empty() ? "nothing matched this game"
                                                        : fetched.error().message));
      }
    } else {
      events.Publish("game.metadata_ready", {{"id", game.id}});
    }
  });
}

}  // namespace mira::metadata
