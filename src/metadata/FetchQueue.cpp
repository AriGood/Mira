#include "metadata/FetchQueue.h"

#include <algorithm>
#include <format>

#include "core/Log.h"
#include "metadata/MetadataFetcher.h"

namespace mira::metadata {

FetchQueue::~FetchQueue() {
  // Finishes what's running, drops the rest: a shutdown mid bulk refresh
  // shouldn't wait out hundreds of fetches.
  std::lock_guard lock(mutex_);
  stopping_ = true;
}

void FetchQueue::Enqueue(const config::Config& config, api::EventBus& events, model::Game game, bool force,
                         bool announce) {
  if (!force && !config.GetBool("metadata.enabled")) return;
  std::lock_guard lock(mutex_);
  const auto waiting = std::find_if(games_.begin(), games_.end(), [&game](const Job& job) {
    return job.game.id == game.id;
  });
  if (waiting != games_.end()) {
    // The newer record (a rename, a new SteamGridDB match) wins.
    waiting->game = std::move(game);
    waiting->announce = waiting->announce || announce;
    return;
  }
  games_.push_back({std::move(game), /*title=*/false, announce});
  StartWorkers(config, events);
}

int FetchQueue::EnqueueTitles(const config::Config& config, api::EventBus& events,
                              std::vector<model::Game> titles) {
  std::lock_guard lock(mutex_);
  int queued = 0;
  for (model::Game& title : titles) {
    if (std::any_of(titles_.begin(), titles_.end(), [&title](const Job& job) { return job.game.id == title.id; })) {
      continue;
    }
    titles_.push_back({std::move(title), /*title=*/true, /*announce=*/false});
    ++queued;
  }
  StartWorkers(config, events);
  return queued;
}

void FetchQueue::WaitIdle() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this] { return games_.empty() && titles_.empty() && running_ == 0; });
}

void FetchQueue::StartWorkers(const config::Config& config, api::EventBus& events) {
  const int wanted = std::min<int>(kWorkers, static_cast<int>(games_.size() + titles_.size()) + running_);
  for (; workers_ < wanted; ++workers_) {
    threads_.Run([this, &config, &events] { Work(config, events); });
  }
}

void FetchQueue::Work(const config::Config& config, api::EventBus& events) {
  for (;;) {
    Job job;
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || (games_.empty() && titles_.empty())) {
        --workers_;
        idle_.notify_all();
        return;
      }
      std::deque<Job>& from = games_.empty() ? titles_ : games_;
      job = std::move(from.front());
      from.pop_front();
      ++running_;
    }
    Run(config, events, job);
    std::lock_guard lock(mutex_);
    --running_;
    idle_.notify_all();
  }
}

void FetchQueue::Run(const config::Config& config, api::EventBus& events, const Job& job) {
  const model::Game& game = job.game;
  if (job.title) {
    if (const Result<void> fetched = FetchCover(config, game); !fetched) {
      log::Debug("no cover for {} {}: {}", game.source, game.source_ref, fetched.error().message);
      events.Publish("library.artwork_failed",
                     {{"source", game.source}, {"ref", game.source_ref}, {"code", fetched.error().code}});
    } else {
      events.Publish("library.artwork_ready", {{"source", game.source}, {"ref", game.source_ref}});
    }
    return;
  }

  if (auto fetched = Fetch(config, game); !fetched) {
    log::Warn("metadata fetch failed for {}: {}", game.id, fetched.error().message);
    events.Publish("game.metadata_failed",
                   {{"id", game.id}, {"code", fetched.error().code}, {"error", fetched.error().message}});
    if (job.announce && fetched.error().code != "no_steamgriddb_key") {
      events.PublishNotification(model::NotifyLevel::Warning,
                                 std::format("No metadata found for \"{}\": {}", game.id,
                                             fetched.error().message.empty() ? "nothing matched this game"
                                                                             : fetched.error().message));
    }
  } else {
    events.Publish("game.metadata_ready", {{"id", game.id}});
  }
}

}  // namespace mira::metadata
