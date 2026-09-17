#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

namespace mira::config {

// Which settings the GUI shows before the user asks for more. Nothing is ever
// removed by a tier, only folded away: total control and a simple first run
// coexist by hiding depth, not withholding it.
//
// The test for each, applied when adding a key rather than argued about
// afterwards:
//
//   Basic     — someone who just wants their games to work may have to
//               change this. It answers "where are my games", "how do they
//               launch", "why does this one have no picture". If not
//               setting it leaves Mira visibly not doing its job, it is
//               Basic, however fiddly the value looks.
//   Advanced  — tuning something that already works: detection weights,
//               timeouts, search paths, where generated files go. Nobody
//               needs it on day one; plenty of people need it eventually.
//   Expert    — you are changing how Mira works, not what it does for you.
//               Wrong values here break things in ways the UI cannot
//               explain.
//
// The bar for Basic is deliberately low. A setting nobody can find is worse
// than a settings screen with one row too many — steamgriddb.api_key sat
// under Expert, and the result was a library of placeholder covers with no
// visible reason.
enum class Tier { Basic, Advanced, Expert };

enum class Type { Bool, Int, Double, String, StringArray, Object };

// Returns an error message when the value is unacceptable. Invalid input is
// rejected with a reason rather than silently clamped.
using Validator = std::function<std::optional<std::string>(const nlohmann::json&)>;

// A validator plus whatever about it a UI can act on.
//
// A bare Validator is a closure: it can reject "maybe" for
// steam.launch_mode with a good message, but GET /v1/config/schema can only
// publish the message *after* the fact, so a settings screen has no way to
// know the key is a two-value enum and renders it as a free-text box. The
// user then types the wrong thing and gets told off for it. Recording the
// allowed values (or the bounds) alongside the check lets the schema
// endpoint describe the shape up front, so the UI can offer a combo box or
// a spin box generically, with no per-key knowledge.
//
// Constructible from a plain Validator, so a check with no describable
// shape (RunnerRef, a path test) is still written as an ordinary lambda.
struct Constraint {
  Validator validate = {};
  std::vector<std::string> one_of;      // empty unless this is an enum
  std::optional<double> minimum;        // unset unless this is ranged
  std::optional<double> maximum;

  Constraint() = default;
  Constraint(Validator validator) : validate(std::move(validator)) {}  // NOLINT: implicit
  explicit operator bool() const { return static_cast<bool>(validate); }
};

struct Entry {
  std::string key;  // dotted, e.g. "scan.debounce_ms"
  Type type;
  nlohmann::json default_value;
  Tier tier;
  std::string doc;
  Constraint constraint = {};  // optional
};

// Every configurable value in the daemon is declared here exactly once. The
// registry drives default materialisation, PATCH validation, the schema the
// frontend builds its settings UI from, `mira config describe`, and the
// generated docs/config.md. A new setting is one declaration and it exists in
// all five places.
class Schema {
public:
  static const Schema& Instance();

  const std::vector<Entry>& Entries() const { return entries_; }
  const Entry* Find(std::string_view key) const;

  // Full default document, nested to match the on-disk layout.
  nlohmann::json Defaults() const;

  // Validates one value against its declared type and validator.
  std::optional<std::string> Validate(std::string_view key, const nlohmann::json& value) const;

  // Validates a whole document, reporting every problem rather than the first.
  std::vector<std::string> ValidateDocument(const nlohmann::json& document) const;

  // "scan.debounce_ms" -> "/scan/debounce_ms"
  static nlohmann::json::json_pointer Pointer(std::string_view dotted_key);

private:
  Schema();
  std::vector<Entry> entries_;
};

std::string_view ToString(Tier tier);
std::string_view ToString(Type type);
std::optional<Tier> TierFromString(std::string_view text);

}  // namespace mira::config
