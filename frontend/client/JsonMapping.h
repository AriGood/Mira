#pragma once

#include <json.hpp>

#include <map>
#include <string>
#include <vector>

#include "Types.h"

// Conversions between mirad's JSON and the structs in Types.h.
//
// Split out from the endpoints themselves because they answer a different
// question: the endpoints know *which* URL to call, these know what the
// bytes mean. They are also the only part of the client with no I/O in
// them, which makes them the part worth reading when a field looks wrong.
namespace mira_gui::mapping {

GameSummary ToGameSummary(const nlohmann::json& entry);
GameDetail ToGameDetail(const nlohmann::json& entry);

// A settings value as a single line of editable text: a bool as
// "true"/"false", a number in its natural form, an array of strings joined
// with ", ". Anything else falls back to its JSON dump.
std::string ToDisplayString(const nlohmann::json& value);

// Flattens GET /v1/config's nested document to the dotted keys Schema
// entries use ("scan.debounce_ms"), skipping the opaque `frontend` table
// (docs/architecture.md) since it isn't part of the schema a settings
// screen renders.
void FlattenConfig(const nlohmann::json& node, const std::string& prefix,
                   std::map<std::string, std::string>& out);

// A trimmed, non-empty comma split — turns a string-array edit box's text
// ("a, b, c") back into entries. The inverse of ToDisplayString's ", " join,
// so round-tripping an unedited field is a no-op.
std::vector<std::string> SplitCommaSeparated(const std::string& text);

// Converts a field's edited display text back to the JSON kind its schema
// `type` calls for. Falls back to sending the raw text for a malformed
// number — the server's own validator explains bad input better than
// guessing client-side would.
nlohmann::json TypedValueFromText(const std::string& type, const std::string& value);

// Drops runners that repeat a `reference` already seen, keeping the first.
//
// GET /v1/runners walks every configured search path, and on a typical
// Arch/Steam setup ~/.steam/steam is a symlink to ~/.local/share/Steam — so
// the same build is discovered twice and reported twice, with identical
// kind/name/version/reference and only `path` differing. docs/api.md says
// `reference` is the string that actually round-trips into `runner_ref`,
// which makes two entries sharing one reference the same runner by
// definition: picking either does exactly the same thing. Collapsing them
// here keeps every runner picker in the UI from offering a choice that
// isn't one.
std::vector<RunnerInfo> DedupeRunnersByReference(std::vector<RunnerInfo> runners);

// Expands a dotted key into nested objects inside `document`, assigning
// `value` at the leaf: "scan.debounce_ms" becomes {"scan":{"debounce_ms":…}},
// which is the shape PATCH /v1/config takes.
void AssignDottedKey(nlohmann::json& document, const std::string& dotted_key,
                     const nlohmann::json& value);

}  // namespace mira_gui::mapping
