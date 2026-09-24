#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

namespace mira::config {

// Global: one value for the whole daemon. PerGame: the same value, but a game
// can also override it (games.toml overrides, PATCH /v1/games/{id}/config).
enum class Scope { Global, PerGame };

enum class Type { Bool, Int, Double, String, StringArray, Object };

using Validator = std::function<std::optional<std::string>(const nlohmann::json&)>;

// A Validator plus the shape it enforces, so /v1/config/schema can publish
// that shape instead of a UI only learning it from a rejected PATCH.
// Constructible from a plain Validator when there's no shape to describe.
struct Constraint {
  Validator validate = {};
  std::vector<std::string> one_of;      // empty unless this is an enum
  std::optional<double> minimum;        // unset unless this is ranged
  std::optional<double> maximum;

  Constraint() = default;
  Constraint(Validator validator) : validate(std::move(validator)) {}  // NOLINT: implicit
  explicit operator bool() const { return static_cast<bool>(validate); }
};

// Declared with designated initializers in Schema.cpp, under the section it
// shows in. See the comment at the top of Schema::Schema() for the pattern.
struct Entry {
  std::string key;    // dotted, e.g. "scan.debounce_ms"
  std::string label;  // what a settings screen calls it
  Type type;
  nlohmann::json default_value;
  Scope scope = Scope::Global;
  std::string doc;
  Constraint constraint = {};  // optional

  bool is_secret = false;      // a credential: mask it
  bool is_runner_ref = false;  // a runner reference: offer a runner picker

  // Set by the section the entry is declared under, never per entry.
  std::string category = {};
  int group = 0;  // index within its category; a divider sits between groups
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

std::string_view ToString(Scope scope);
std::string_view ToString(Type type);

}  // namespace mira::config
