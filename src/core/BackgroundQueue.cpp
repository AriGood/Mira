#include "core/BackgroundQueue.h"

#include <chrono>
#include <exception>

#include "core/Log.h"

namespace mira {

BackgroundQueue::~BackgroundQueue() {
  std::lock_guard lock(mutex_);
  pending_.clear();  // each future's destructor blocks until its task finishes
}

void BackgroundQueue::Run(std::function<void()> task) {
  std::lock_guard lock(mutex_);
  std::erase_if(pending_, [](std::future<void>& fetch) {
    return fetch.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  });
  // Caught here rather than left to the future. A task that throws stores
  // its exception in the future, and this queue discards futures without
  // ever calling get() — so the throw produced no log line, no event and no
  // result, and the work simply appeared not to have happened. That is
  // exactly how a json::type_error in the metadata fetcher hid: three games
  // silently had no metadata and nothing anywhere said why.
  pending_.push_back(std::async(std::launch::async, [task = std::move(task)] {
    try {
      task();
    } catch (const std::exception& error) {
      log::Error("background task threw: {}", error.what());
    } catch (...) {
      log::Error("background task threw a non-std exception");
    }
  }));
}

}  // namespace mira
