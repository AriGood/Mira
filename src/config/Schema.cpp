#include "config/Schema.h"

#include <algorithm>
#include <format>

#include "core/Strings.h"

namespace mira::config {
namespace {

using nlohmann::json;

Validator Range(double min, double max) {
  return [min, max](const json& value) -> std::optional<std::string> {
    if (!value.is_number()) return "expected a number";
    const double number = value.get<double>();
    if (number < min || number > max) {
      return std::format("must be between {} and {}", min, max);
    }
    return std::nullopt;
  };
}

Validator OneOf(std::vector<std::string> allowed) {
  return [allowed = std::move(allowed)](const json& value) -> std::optional<std::string> {
    if (!value.is_string()) return "expected a string";
    const std::string text = value.get<std::string>();
    if (std::ranges::find(allowed, text) == allowed.end()) {
      std::string list;
      for (const std::string& option : allowed) {
        if (!list.empty()) list += ", ";
        list += option;
      }
      return std::format("must be one of: {}", list);
    }
    return std::nullopt;
  };
}

// A runner reference is "kind:name", where name may be "latest".
Validator RunnerRef() {
  return [](const json& value) -> std::optional<std::string> {
    if (!value.is_string()) return "expected a string";
    const std::string text = value.get<std::string>();
    if (text.empty()) return "must not be empty";
    if (text.find(':') == std::string::npos) return "must be \"kind:name\", e.g. \"wine:system\"";
    return std::nullopt;
  };
}

Validator NonEmptyString() {
  return [](const json& value) -> std::optional<std::string> {
    if (!value.is_string()) return "expected a string";
    if (value.get<std::string>().empty()) return "must not be empty";
    return std::nullopt;
  };
}

}  // namespace

Schema::Schema() {
  entries_ = {
      {"socket_path", Type::String, "$XDG_RUNTIME_DIR/mira/mirad.sock", Tier::Expert,
       "Unix socket the daemon listens on. The frontend and CLI must agree with this.",
       NonEmptyString()},

      {"log.level", Type::String, "info", Tier::Advanced,
       "Logging verbosity: debug, info, warn or error.",
       OneOf({"debug", "info", "warn", "error"})},

      {"library_roots", Type::StringArray, json::array({"~/Games"}), Tier::Basic,
       "Folders watched for new games. Dropping a game folder into one of these is all "
       "that is required to add it."},

      {"prefix_root", Type::String, "~/Games/prefix", Tier::Basic,
       "Where per-game data directories (Wine/Proton prefixes) are created. Always "
       "excluded from scanning, wherever it points."},

      {"auto_setup", Type::Bool, true, Tier::Basic,
       "Configure and provision newly detected games automatically. With this off, games "
       "are detected but wait for the frontend to configure them."},

      {"open_config_on_add", Type::Bool, true, Tier::Basic,
       "Ask the frontend to open its configuration menu when a game is added, so the "
       "auto-detected settings can be reviewed."},

      {"default_runner.windows", Type::String, "auto", Tier::Basic,
       "Runner for Windows games as \"kind:name\", or \"auto\" to pick the best installed "
       "runner (Proton via umu if present, otherwise Wine)."},

      {"default_runner.native", Type::String, "native:native", Tier::Expert,
       "Runner for native Linux games.", RunnerRef()},

      {"runner_search_paths", Type::StringArray,
       json::array({"~/.steam/steam/compatibilitytools.d",
                    "~/.local/share/Steam/compatibilitytools.d",
                    "~/.local/share/mira/runners"}),
       Tier::Advanced, "Directories scanned for installed Proton builds."},

      {"prefix_provider", Type::String, "plain", Tier::Advanced,
       "How a new prefix directory is created. \"plain\" lets the runner initialise it; "
       "\"template\" clones prefix_template, which is far faster on btrfs/xfs.",
       OneOf({"plain", "template"})},

      {"prefix_template", Type::String, "", Tier::Advanced,
       "An already-initialised prefix to clone when prefix_provider is \"template\". "
       "Cloned with reflinks where the filesystem supports them."},

      {"command_wrappers", Type::StringArray, json::array(), Tier::Advanced,
       "Wrappers applied to the launch command in order, e.g. [\"mangohud\"]. Each entry "
       "is a command that receives the game's command line as its arguments."},

      {"scan.debounce_ms", Type::Int, 3000, Tier::Advanced,
       "How long a new folder must stop changing before it is scanned. Raise it if games "
       "arrive over a slow network share.",
       Range(0, 600000)},

      {"scan.max_depth", Type::Int, 4, Tier::Advanced,
       "How deep to search inside a game folder for executables.", Range(1, 16)},

      {"scan.periodic_interval_s", Type::Int, 0, Tier::Expert,
       "Seconds between full rescans. 0 disables them, which is the default: inotify is "
       "authoritative and a timer would cost idle wakeups for nothing.",
       Range(0, 86400)},

      {"scan.ignore_globs", Type::StringArray,
       json::array({".*", "*/Redist*", "*/DirectX*", "*/_CommonRedist*", "*/DotNet*"}),
       Tier::Advanced, "Paths matching these globs are never treated as games."},

      {"detect.rules", Type::StringArray,
       json::array({"deny_patterns", "name_similarity", "depth", "shallowest"}),
       Tier::Expert,
       "Scoring rules applied to candidate executables, in order. Removing a rule "
       "disables it."},

      {"detect.name_match_bonus", Type::Double, 3.0, Tier::Expert,
       "Score added when an executable's name resembles its folder's name.",
       Range(0.0, 100.0)},

      {"detect.depth_penalty", Type::Double, 0.5, Tier::Expert,
       "Score subtracted per directory level, favouring executables near the top.",
       Range(0.0, 100.0)},

      {"detect.low_confidence_threshold", Type::Double, 0.5, Tier::Advanced,
       "Below this confidence a game is flagged for review. It is still configured and "
       "still launchable; the frontend just highlights it.",
       Range(0.0, 1.0)},

      {"detect.deny_name_patterns", Type::StringArray,
       json::array({"unins*", "setup*", "vcredist*", "dxsetup*", "*crashreport*", "dotnet*",
                    "directx*", "*redist*", "touchup*", "*launcher_installer*"}),
       Tier::Advanced,
       "Executables matching these globs are heavily penalised: they are installers and "
       "helpers rather than games."},

      {"events.sse_keepalive_s", Type::Int, 0, Tier::Expert,
       "Seconds between keepalive comments on the event stream. 0 disables them; a Unix "
       "socket does not need them and a timer would cost idle wakeups.",
       Range(0, 3600)},
  };
}

const Schema& Schema::Instance() {
  static const Schema instance;
  return instance;
}

const Entry* Schema::Find(std::string_view key) const {
  const auto it = std::ranges::find(entries_, key, &Entry::key);
  return it == entries_.end() ? nullptr : &*it;
}

nlohmann::json::json_pointer Schema::Pointer(std::string_view dotted_key) {
  std::string pointer;
  for (const char c : dotted_key) pointer += (c == '.') ? '/' : c;
  return nlohmann::json::json_pointer("/" + pointer);
}

json Schema::Defaults() const {
  json document = json::object();
  for (const Entry& entry : entries_) document[Pointer(entry.key)] = entry.default_value;
  return document;
}

std::optional<std::string> Schema::Validate(std::string_view key, const json& value) const {
  const Entry* entry = Find(key);
  if (entry == nullptr) return std::format("unknown setting \"{}\"", key);

  const bool type_ok = [&] {
    switch (entry->type) {
      case Type::Bool:        return value.is_boolean();
      case Type::Int:         return value.is_number_integer();
      case Type::Double:      return value.is_number();
      case Type::String:      return value.is_string();
      case Type::StringArray: return value.is_array() && std::ranges::all_of(value, [](const json& item) {
                                       return item.is_string();
                                     });
      case Type::Object:      return value.is_object();
    }
    return false;
  }();
  if (!type_ok) return std::format("expected {}", ToString(entry->type));

  if (entry->validator) return entry->validator(value);
  return std::nullopt;
}

std::vector<std::string> Schema::ValidateDocument(const json& document) const {
  std::vector<std::string> problems;
  for (const Entry& entry : entries_) {
    const auto pointer = Pointer(entry.key);
    if (!document.contains(pointer)) continue;  // absent means "use the default"
    if (auto problem = Validate(entry.key, document[pointer])) {
      problems.push_back(std::format("{}: {}", entry.key, *problem));
    }
  }
  return problems;
}

std::string_view ToString(Tier tier) {
  switch (tier) {
    case Tier::Basic:    return "basic";
    case Tier::Advanced: return "advanced";
    case Tier::Expert:   return "expert";
  }
  return "basic";
}

std::string_view ToString(Type type) {
  switch (type) {
    case Type::Bool:        return "a boolean";
    case Type::Int:         return "an integer";
    case Type::Double:      return "a number";
    case Type::String:      return "a string";
    case Type::StringArray: return "an array of strings";
    case Type::Object:      return "an object";
  }
  return "a value";
}

std::optional<Tier> TierFromString(std::string_view text) {
  if (text == "basic") return Tier::Basic;
  if (text == "advanced") return Tier::Advanced;
  if (text == "expert") return Tier::Expert;
  return std::nullopt;
}

}  // namespace mira::config
