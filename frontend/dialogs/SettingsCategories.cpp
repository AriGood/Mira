#include "SettingsCategories.h"

#include <algorithm>
#include <array>
#include <map>

namespace mira_gui::settings {
// The one schema key GET /v1/config/schema can't itself mark as "this is a
// runner reference" (docs/config schema has no such concept) — see the
// class comment in SettingsDialog.h for why it gets a picker (GET
// /v1/runners) instead of a plain text box like everything else here.
// "default_runner.native" is deliberately NOT included: its schema
// validator (config::RunnerRef in Schema.cpp) requires a literal
// "kind:name" and rejects "auto", and there is exactly one native runner
// anyway (RunnerRegistry has nothing to pick "best" between for it) — so a
// picker would only ever offer an option ("Auto") guaranteed to fail
// validation.
constexpr std::array<const char*, 1> kRunnerKeys = {"default_runner.windows"};

bool IsRunnerKey(const std::string& key) {
  return std::ranges::find(kRunnerKeys, key) != kRunnerKeys.end();
}

// Groups rows for readability — see the class comment in SettingsDialog.h.
// Everything currently in src/config/Schema.cpp is named explicitly; a
// schema key added later without a matching entry here still lands
// somewhere sensible via the dotted-prefix guess below, never disappears.
QString CategoryFor(const std::string& key) {
  static const std::map<std::string, QString> kOverrides = {
      {"library_roots", "Library"},        {"prefix_root", "Library"},
      {"prefix_provider", "Library"},      {"prefix_template", "Library"},
      {"auto_setup", "Library"},           {"open_config_on_add", "Library"},
      {"command_wrappers", "Library"},     {"default_runner.windows", "Runners"},
      {"default_runner.native", "Runners"}, {"runner_search_paths", "Runners"},
      {"wine_search_paths", "Runners"},    {"socket_path", "Advanced"},
      {"log.level", "Advanced"},           {"events.sse_keepalive_s", "Advanced"},
      {"library.remove_missing", "Library"},
  };
  if (const auto it = kOverrides.find(key); it != kOverrides.end()) return it->second;

  const size_t dot = key.find('.');
  if (dot == std::string::npos) return "General";
  const std::string prefix = key.substr(0, dot);
  if (prefix == "detect") return "Detection";
  if (prefix == "scan") return "Scanning";
  if (prefix == "default_runner") return "Runners";
  if (prefix == "log" || prefix == "events") return "Advanced";
  if (prefix == "launch") return "Launching";
  if (prefix == "desktop_entries") return "Desktop Entries";
  if (prefix == "library") return "Library";

  QString label = QString::fromStdString(prefix).replace('_', ' ');
  if (!label.isEmpty()) label[0] = label[0].toUpper();
  return label;
}

// Categories appear in this order when present; anything else (a future,
// unmapped prefix) is appended alphabetically after — see CategoryFor.
const QStringList& CategoryOrder() {
  static const QStringList order = {"Library",    "Runners",         "Launching",
                                     "Detection",  "Scanning",        "Desktop Entries",
                                     "Advanced",   "General"};
  return order;
}
}  // namespace mira_gui::settings
