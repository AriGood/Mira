#include <doctest.h>

#include <json.hpp>

#include "client/JsonMapping.h"

using nlohmann::json;
using namespace mira_gui;

// These cover the frontend's half of the contract in docs/api.md: what the
// UI believes each field means. A mismatch here is the kind of bug that
// shows up as a blank column rather than an error, so the interesting cases
// are the absent and the wrongly-typed ones, not the happy path.

TEST_CASE("ToGameSummary reads the fields a library row shows") {
  const json entry = json::parse(R"({
    "id": "animal-well", "name": "Animal Well", "status": "ready",
    "platform": "windows", "runner_ref": "proton_umu:GE-Proton11-7",
    "last_error": "", "reviewed": true, "confidence": 0.75,
    "last_played_at": 1789620825, "play_seconds": 4210
  })");

  const GameSummary game = mapping::ToGameSummary(entry);
  CHECK(game.id == "animal-well");
  CHECK(game.name == "Animal Well");
  CHECK(game.status == "ready");
  CHECK(game.platform == "windows");
  CHECK(game.runner_ref == "proton_umu:GE-Proton11-7");
  CHECK(game.reviewed);
  CHECK(game.confidence == doctest::Approx(0.75));
  REQUIRE(game.last_played_at.has_value());
  CHECK(*game.last_played_at == 1789620825);
  CHECK(game.play_seconds == 4210);
}

TEST_CASE("ToGameSummary tolerates a record missing every optional field") {
  // mirad omits nothing today, but a summary is also built from an SSE
  // payload (ParseGameSummary), and an older daemon or a future trimmed
  // event must not produce garbage — every absent field falls back rather
  // than throwing.
  const GameSummary game = mapping::ToGameSummary(json::parse(R"({"id": "x"})"));
  CHECK(game.id == "x");
  CHECK(game.name.empty());
  CHECK(game.status.empty());
  CHECK_FALSE(game.reviewed);
  CHECK(game.confidence == doctest::Approx(0.0));
  CHECK(game.play_seconds == 0);
  CHECK_FALSE(game.last_played_at.has_value());
}

TEST_CASE("ToGameSummary treats a null last_played_at as never played") {
  // The distinction the UI actually renders: mirad sends null (not a
  // missing key, not 0) for a game that has never run, and "Never" has to
  // survive that. A 0 would be a real timestamp — 1970 — so these must not
  // collapse into each other.
  const GameSummary never =
      mapping::ToGameSummary(json::parse(R"({"id": "x", "last_played_at": null})"));
  CHECK_FALSE(never.last_played_at.has_value());

  const GameSummary epoch =
      mapping::ToGameSummary(json::parse(R"({"id": "x", "last_played_at": 0})"));
  REQUIRE(epoch.last_played_at.has_value());
  CHECK(*epoch.last_played_at == 0);
}

TEST_CASE("ToGameDetail keeps runner_config and env as JSON text") {
  // They are arbitrary objects with no fixed shape to build widgets for, so
  // the dialog round-trips them as text — which means an absent one has to
  // become "{}" and not an empty string, or saving an untouched game would
  // send an unparsable body.
  const GameDetail detail = mapping::ToGameDetail(json::parse(R"({"id": "x"})"));
  CHECK(detail.runner_config_json == "{}");
  CHECK(detail.env_json == "{}");

  const GameDetail with_env = mapping::ToGameDetail(json::parse(R"({
    "id": "x", "env": {"DXVK_HUD": "fps"}
  })"));
  CHECK(json::parse(with_env.env_json)["DXVK_HUD"] == "fps");
}

TEST_CASE("ToGameDetail reads every candidate, including installer flags") {
  const GameDetail detail = mapping::ToGameDetail(json::parse(R"({
    "id": "x",
    "candidates": [
      {"rel_path": "setup.exe", "kind": "windows", "score": 1.5,
       "chosen": false, "is_installer": true},
      {"rel_path": "game.exe", "kind": "windows", "score": 4.0, "chosen": true}
    ]
  })"));

  REQUIRE(detail.candidates.size() == 2);
  CHECK(detail.candidates[0].rel_path == "setup.exe");
  CHECK(detail.candidates[0].is_installer);
  CHECK_FALSE(detail.candidates[0].chosen);
  CHECK(detail.candidates[1].chosen);
  CHECK_FALSE(detail.candidates[1].is_installer);  // absent means false, not unset
  CHECK(detail.candidates[1].score == doctest::Approx(4.0));
}

TEST_CASE("ToDisplayString renders each schema type as one editable line") {
  CHECK(mapping::ToDisplayString(json(true)) == "true");
  CHECK(mapping::ToDisplayString(json(false)) == "false");
  CHECK(mapping::ToDisplayString(json(42)) == "42");
  CHECK(mapping::ToDisplayString(json("~/Games")) == "~/Games");
  CHECK(mapping::ToDisplayString(json::parse(R"(["a", "b"])")) == "a, b");
  CHECK(mapping::ToDisplayString(json::array()).empty());
}

TEST_CASE("ToDisplayString does not quote a string inside an array") {
  // The array form feeds a comma-separated edit box that SplitCommaSeparated
  // reads back, so a quoted entry here would survive a save as a literal
  // pair of quote characters in the config.
  CHECK(mapping::ToDisplayString(json::parse(R"(["/home/me/Games"])")) == "/home/me/Games");
}

TEST_CASE("SplitCommaSeparated is the inverse of the array display form") {
  const std::vector<std::string> items = mapping::SplitCommaSeparated(" a , b ,c ");
  REQUIRE(items.size() == 3);
  CHECK(items[0] == "a");
  CHECK(items[1] == "b");
  CHECK(items[2] == "c");

  CHECK(mapping::SplitCommaSeparated("").empty());
  CHECK(mapping::SplitCommaSeparated("   ").empty());
  CHECK(mapping::SplitCommaSeparated(",,").empty());  // empty entries are dropped, not sent
}

TEST_CASE("An array round-trips unchanged through display and split") {
  const json original = json::parse(R"(["~/Games", "/mnt/games"])");
  const std::vector<std::string> back =
      mapping::SplitCommaSeparated(mapping::ToDisplayString(original));
  CHECK(json(back) == original);
}

TEST_CASE("TypedValueFromText converts by schema type, not by looks") {
  // The schema type is authoritative: "5" under a string key must stay a
  // string, or a PATCH would change the value's JSON kind behind the user.
  CHECK(mapping::TypedValueFromText("a boolean", "true") == json(true));
  CHECK(mapping::TypedValueFromText("a boolean", "anything else") == json(false));
  CHECK(mapping::TypedValueFromText("an integer", "1500") == json(1500));
  CHECK(mapping::TypedValueFromText("a number", "0.5") == json(0.5));
  CHECK(mapping::TypedValueFromText("a string", "5") == json("5"));
  CHECK(mapping::TypedValueFromText("an array of strings", "a, b") == json::parse(R"(["a","b"])"));
}

TEST_CASE("TypedValueFromText sends a malformed number as text") {
  // Deliberate: the server's own validator explains the bad input better
  // than a client-side guess would, so the typo is forwarded rather than
  // silently turned into 0.
  CHECK(mapping::TypedValueFromText("an integer", "twelve") == json("twelve"));
  CHECK(mapping::TypedValueFromText("a number", "") == json(""));
}

TEST_CASE("FlattenConfig produces the dotted keys the schema uses") {
  std::map<std::string, std::string> out;
  mapping::FlattenConfig(json::parse(R"({
    "auto_setup": true,
    "scan": {"debounce_ms": 1500},
    "default_runner": {"windows": "proton_umu:auto"}
  })"),
                         "", out);

  CHECK(out["auto_setup"] == "true");
  CHECK(out["scan.debounce_ms"] == "1500");
  CHECK(out["default_runner.windows"] == "proton_umu:auto");
}

TEST_CASE("FlattenConfig skips the opaque frontend table") {
  // settings.toml's [frontend] is passthrough storage the backend never
  // validates (docs/architecture.md); showing it in a schema-generated
  // settings screen would offer edits no schema entry describes.
  std::map<std::string, std::string> out;
  mapping::FlattenConfig(json::parse(R"({
    "auto_setup": true,
    "frontend": {"window_width": 1180}
  })"),
                         "", out);

  CHECK(out.count("auto_setup") == 1);
  CHECK(out.count("frontend.window_width") == 0);
}

TEST_CASE("FlattenConfig only skips frontend at the top level") {
  // A nested key that happens to be named "frontend" is a real setting.
  std::map<std::string, std::string> out;
  mapping::FlattenConfig(json::parse(R"({"desktop_entries": {"frontend": "x"}})"), "", out);
  CHECK(out["desktop_entries.frontend"] == "x");
}

TEST_CASE("AssignDottedKey rebuilds the nesting PATCH /v1/config expects") {
  json document = json::object();
  mapping::AssignDottedKey(document, "scan.debounce_ms", json(1500));
  mapping::AssignDottedKey(document, "auto_setup", json(true));
  CHECK(document == json::parse(R"({"scan": {"debounce_ms": 1500}, "auto_setup": true})"));
}

TEST_CASE("AssignDottedKey merges two keys sharing a prefix") {
  // Both edits have to survive: writing the second must not replace the
  // object the first one created, or saving two settings in one group would
  // silently drop one of them.
  json document = json::object();
  mapping::AssignDottedKey(document, "default_runner.windows", json("proton_umu:auto"));
  mapping::AssignDottedKey(document, "default_runner.native", json("native:native"));
  CHECK(document["default_runner"].size() == 2);
  CHECK(document["default_runner"]["windows"] == "proton_umu:auto");
  CHECK(document["default_runner"]["native"] == "native:native");
}
