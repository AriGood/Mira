#include "core/BackgroundQueue.h"

#include <chrono>

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
  pending_.push_back(std::async(std::launch::async, std::move(task)));
}

}  // namespace mira
