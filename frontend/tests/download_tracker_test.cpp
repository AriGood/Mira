#include <doctest.h>

#include "ui/DownloadTracker.h"

using namespace mira_gui;
using State = DownloadTracker::State;

namespace {

State StateOf(const DownloadTracker& tracker, const QString& key) {
  const DownloadTracker::Entry* entry = tracker.Find(key);
  REQUIRE(entry != nullptr);
  return entry->state;
}

}  // namespace

TEST_CASE("DownloadTracker follows a store install from start to finish") {
  DownloadTracker tracker;
  // Named up front, so it doesn't ask mirad for the store's titles.
  tracker.NoteTitle("gog", "1207658924", "Alan Wake");

  CHECK(tracker.HandleEvent("library.install.started", R"({"source": "gog", "ref": "1207658924", "update": false})"));
  CHECK(tracker.RunningCount() == 1);
  const DownloadTracker::Entry* entry = tracker.Find("gog:1207658924");
  REQUIRE(entry != nullptr);
  CHECK(tracker.NameFor(*entry).toStdString() == "Alan Wake");
  CHECK(DownloadTracker::GameIdFor(*entry).toStdString() == "gog-1207658924");

  tracker.HandleEvent("library.install.failed",
                      R"({"source": "gog", "ref": "1207658924", "update": false, "error": "disk full"})");
  CHECK(StateOf(tracker, "gog:1207658924") == State::Failed);
  CHECK(tracker.Find("gog:1207658924")->error.toStdString() == "disk full");
  CHECK(tracker.RunningCount() == 0);
}

TEST_CASE("DownloadTracker keeps the newest activity first and tells kinds apart") {
  DownloadTracker tracker;
  tracker.source_name = [](const QString& source) { return source == "battlenet" ? "Battle.net" : source; };

  tracker.HandleEvent("launcher.install.started", R"({"id": "battlenet"})");
  tracker.HandleEvent("gog.setup.started", R"({"tag": "v1.1"})");
  tracker.HandleEvent("humble.download.started", R"({"bundle_key": "abc"})");
  tracker.HandleEvent("runners.download.started", R"({"kind": "proton", "tag": "GE-Proton10-4"})");
  CHECK_FALSE(tracker.HandleEvent("game.added", R"({"id": "x"})"));

  REQUIRE(tracker.Entries().size() == 4);
  CHECK(tracker.Entries()[0].kind == DownloadTracker::Kind::Runner);
  CHECK(tracker.NameFor(tracker.Entries()[0]).toStdString() == "GE-Proton10-4");
  CHECK(tracker.Entries()[3].kind == DownloadTracker::Kind::Launcher);
  CHECK(tracker.NameFor(tracker.Entries()[3]).toStdString() == "Battle.net");
  CHECK(tracker.NameFor(*tracker.Find("tool:gog")).toStdString() == "gogdl");

  // Moves back to the top when it changes.
  tracker.HandleEvent("launcher.install.finished", R"({"id": "battlenet"})");
  CHECK(tracker.Entries()[0].key.toStdString() == "launcher:battlenet");
  CHECK(tracker.RunningCount() == 3);

  tracker.ClearFinished();
  CHECK(tracker.Entries().size() == 3);
  CHECK(tracker.Find("launcher:battlenet") == nullptr);
}
