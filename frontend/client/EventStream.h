#pragma once

#include <QObject>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace mira_gui {

// A long-lived connection to GET /v1/events (docs/api.md), which streams
// Server-Sent Events for `game.added`/`game.updated`/`game.removed`/
// `game.state` as the daemon does its work. `mira watch` (src/cli/main.cpp)
// is the reference client this mirrors: a streaming GET split on blank
// lines.
//
// Unlike MiradClient's one-shot calls this holds state — the last event id,
// for replay across reconnects — and its background thread runs for as long
// as the connection keeps getting reconnected, so it is an instance rather
// than a static call.
class EventStream {
public:
  EventStream();
  ~EventStream();
  EventStream(const EventStream&) = delete;
  EventStream& operator=(const EventStream&) = delete;

  // Starts (or restarts) the background connection. Delivers each event's
  // type and raw JSON data on the main thread. Reconnects with a fixed
  // backoff on any drop — mirad itself may restart independently of the
  // frontend — replaying via Last-Event-ID so a reconnect doesn't miss
  // events still in mirad's 500-event buffer.
  //
  // Destroying the stream stops it. It used to be left running forever on
  // the theory that the process has exactly one window for its whole
  // lifetime, which stopped being true the moment a second window could be
  // opened, and was never safe anyway — see async::Deliver.
  void Start(QObject* context, std::function<void(std::string type, std::string data)> on_event);

private:
  // Shared with the background thread rather than owned by it, so that
  // destroying the stream can signal a thread that outlives this object.
  struct State {
    std::atomic<bool> stopped{false};
  };
  std::shared_ptr<State> state_;
};

}  // namespace mira_gui
