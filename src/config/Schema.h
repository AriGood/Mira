#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

namespace mira::config {

// Which settings the GUI shows before the user asks for more. A tier only
// folds a setting away, never removes it. Basic = needed to make Mira visibly
// work (e.g. steamgriddb.api_key); Advanced = tuning something that already
// works; Expert = changes how Mira works, not just what it does.
enum class Tier { Basic, Advanced, Expert };

enum class Type { Bool, Int, Double, String, StringArray, Object };

// Returns an error message when the value is unacceptable. Invalid input is
// rejected with a reason rather than silently clamped.
using Validator = std::function<std::optional<std::string>(const nlohmann::json&)>;

struct Entry {
  std::string key;  // dotted, e.g. "scan.debounce_ms"
  Type type;
  nlohmann::json default_value;
  Tier tier;
  std::string doc;
  Validator validator = {};  // optional
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
