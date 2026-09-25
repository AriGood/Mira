#include <doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "api/EventBus.h"

using namespace mira;

TEST_CASE("WaitNext delivers an event published concurrently, with no polling") {
  // This is the exact shape the SSE handler and the API thread run in
  // production: one thread blocked in WaitNext, another calling Publish.
  // Run under the tsan preset, this is what actually exercises the
  // condition-variable handoff that Server.cpp relies on.
  api::EventBus bus;
  std::atomic<bool> stop{false};
  std::optional<model::Event> received;

  std::thread subscriber([&] { received = bus.WaitNext(0, stop, std::chrono::seconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let it start waiting
  bus.Publish("game.added", {{"id", "celeste"}});
  subscriber.join();

  REQUIRE(received.has_value());
  CHECK(received->type == "game.added");
  CHECK(received->payload.value("id", "") == "celeste");
}

TEST_CASE("WaitNext times out cleanly with no event and no stop") {
  api::EventBus bus;
  std::atomic<bool> stop{false};
  auto result = bus.WaitNext(0, stop, std::chrono::milliseconds(50));
  CHECK_FALSE(result.has_value());
}

TEST_CASE("WaitNext unblocks immediately when stop is set") {
  api::EventBus bus;
  std::atomic<bool> stop{true};
  const auto start = std::chrono::steady_clock::now();
  auto result = bus.WaitNext(0, stop, std::chrono::seconds(30));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK_FALSE(result.has_value());
  CHECK(elapsed < std::chrono::seconds(1));
}

TEST_CASE("many publishers and many subscribers race safely") {
  // Not a correctness oracle for ordering (only the ring buffer's own mutex
  // guarantees that) — this exists to give TSan a genuinely concurrent
  // workload against Publish/WaitNext/Since together.
  api::EventBus bus;
  std::atomic<bool> stop{false};
  std::atomic<int> delivered{0};

  std::vector<std::thread> subscribers;
  for (int i = 0; i < 4; ++i) {
    subscribers.emplace_back([&] {
      std::int64_t after = 0;
      while (!stop.load()) {
        if (auto event = bus.WaitNext(after, stop, std::chrono::milliseconds(100))) {
          after = event->id;
          delivered.fetch_add(1);
        }
      }
    });
  }

  std::vector<std::thread> publishers;
  for (int i = 0; i < 4; ++i) {
    publishers.emplace_back([&, i] {
      for (int n = 0; n < 25; ++n) bus.Publish("stress", {{"from", i}, {"n", n}});
    });
  }
  for (auto& t : publishers) t.join();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true);
  for (auto& t : subscribers) t.join();

  const auto all = bus.Since(0);
  REQUIRE(all.size() == 100);
  CHECK(all.back().id - all.front().id == 99);
  CHECK(delivered.load() > 0);
  CHECK(bus.Since(0).size() <= 100);  // ring buffer capacity may have trimmed some
}
