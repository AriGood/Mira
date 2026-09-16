#include "api/EventBus.h"

namespace mira::api {

model::Event EventBus::Publish(std::string type, nlohmann::json payload) {
  model::Event event;
  {
    std::lock_guard lock(mutex_);
    event = {next_id_++, model::NowSeconds(), std::move(type), std::move(payload)};
    events_.push_back(event);
    while (events_.size() > capacity_) events_.pop_front();
  }
  cv_.notify_all();
  return event;
}

std::optional<model::Event> EventBus::WaitNext(std::int64_t after_id, const std::atomic<bool>& stop,
                                                std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  const bool got = cv_.wait_for(lock, timeout, [&] {
    return stop.load(std::memory_order_relaxed) ||
           (!events_.empty() && events_.back().id > after_id);
  });
  if (!got || stop.load(std::memory_order_relaxed)) return std::nullopt;
  for (const model::Event& event : events_) {
    if (event.id > after_id) return event;
  }
  return std::nullopt;  // unreachable given the wait predicate, but keeps the type honest
}

std::vector<model::Event> EventBus::Since(std::int64_t after_id) const {
  std::lock_guard lock(mutex_);
  std::vector<model::Event> out;
  for (const model::Event& event : events_) {
    if (event.id > after_id) out.push_back(event);
  }
  return out;
}

std::int64_t EventBus::LatestId() const {
  std::lock_guard lock(mutex_);
  return events_.empty() ? 0 : events_.back().id;
}

}  // namespace mira::api
