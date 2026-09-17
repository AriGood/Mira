#include "JsonMapping.h"

#include <cstdint>
#include <string>

namespace mira_gui::mapping {

using nlohmann::json;

GameSummary ToGameSummary(const json& entry) {
  GameSummary game;
  game.id = entry.value("id", std::string());
  game.name = entry.value("name", std::string());
  game.status = entry.value("status", std::string());
  game.platform = entry.value("platform", std::string());
  game.runner_ref = entry.value("runner_ref", std::string());
  game.last_error = entry.value("last_error", std::string());
  game.reviewed = entry.value("reviewed", false);
  game.confidence = entry.value("confidence", 0.0);
  if (entry.contains("last_played_at") && entry["last_played_at"].is_number()) {
    game.last_played_at = entry["last_played_at"].get<std::int64_t>();
  }
  game.play_seconds = entry.value("play_seconds", std::int64_t{0});
  return game;
}

GameDetail ToGameDetail(const json& entry) {
  GameDetail game;
  game.id = entry.value("id", std::string());
  game.name = entry.value("name", std::string());
  game.status = entry.value("status", std::string());
  game.platform = entry.value("platform", std::string());
  game.install_path = entry.value("install_path", std::string());
  game.exe_path = entry.value("exe_path", std::string());
  game.args = entry.value("args", std::string());
  game.working_dir = entry.value("working_dir", std::string());
  game.runner_ref = entry.value("runner_ref", std::string());
  game.data_dir = entry.value("data_dir", std::string());
  game.last_error = entry.value("last_error", std::string());
  game.reviewed = entry.value("reviewed", false);
  game.confidence = entry.value("confidence", 0.0);
  if (entry.contains("last_played_at") && entry["last_played_at"].is_number()) {
    game.last_played_at = entry["last_played_at"].get<std::int64_t>();
  }
  game.play_seconds = entry.value("play_seconds", std::int64_t{0});
  game.runner_config_json = entry.value("runner_config", json::object()).dump(2);
  game.env_json = entry.value("env", json::object()).dump(2);

  for (const json& candidate : entry.value("candidates", json::array())) {
    GameDetail::Candidate c;
    c.rel_path = candidate.value("rel_path", std::string());
    c.kind = candidate.value("kind", std::string());
    c.score = candidate.value("score", 0.0);
    c.chosen = candidate.value("chosen", false);
    c.is_installer = candidate.value("is_installer", false);
    game.candidates.push_back(std::move(c));
  }
  return game;
}

std::string ToDisplayString(const json& value) {
  if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
  if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
  if (value.is_number()) return value.dump();
  if (value.is_string()) return value.get<std::string>();
  if (value.is_array()) {
    std::string joined;
    for (const json& item : value) {
      if (!joined.empty()) joined += ", ";
      joined += item.is_string() ? item.get<std::string>() : item.dump();
    }
    return joined;
  }
  return value.dump();
}

void FlattenConfig(const json& node, const std::string& prefix,
                   std::map<std::string, std::string>& out) {
  if (!node.is_object()) {
    out[prefix] = ToDisplayString(node);
    return;
  }
  for (const auto& [key, value] : node.items()) {
    if (prefix.empty() && key == "frontend") continue;  // opaque, not part of the schema
    const std::string dotted = prefix.empty() ? key : prefix + "." + key;
    if (value.is_object()) {
      FlattenConfig(value, dotted, out);
    } else {
      out[dotted] = ToDisplayString(value);
    }
  }
}

std::vector<std::string> SplitCommaSeparated(const std::string& text) {
  std::vector<std::string> items;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos) end = text.size();
    std::string item = text.substr(start, end - start);
    const size_t first = item.find_first_not_of(" \t\r\n");
    const size_t last = item.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) items.push_back(item.substr(first, last - first + 1));
    start = end + 1;
  }
  return items;
}

json TypedValueFromText(const std::string& type, const std::string& value) {
  if (type == "a boolean") return value == "true";
  if (type == "an integer") {
    try {
      return static_cast<std::int64_t>(std::stoll(value));
    } catch (...) {
      return value;
    }
  }
  if (type == "a number") {
    try {
      return std::stod(value);
    } catch (...) {
      return value;
    }
  }
  if (type == "an array of strings") return SplitCommaSeparated(value);
  return value;
}

void AssignDottedKey(json& document, const std::string& dotted_key, const json& value) {
  json* cursor = &document;
  size_t start = 0;
  while (true) {
    const size_t dot = dotted_key.find('.', start);
    const std::string segment =
        dotted_key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (dot == std::string::npos) {
      (*cursor)[segment] = value;
      return;
    }
    cursor = &(*cursor)[segment];
    start = dot + 1;
  }
}

}  // namespace mira_gui::mapping
