#include "proc/Stats.h"

#include <format>
#include <fstream>

#include <toml.hpp>

#include "core/Log.h"
#include "core/TomlJson.h"

namespace mira::proc {
namespace {
using nlohmann::json;

json SessionToJson(const SessionRecord& record) {
  json j = json::object();
  j["game_id"] = record.game_id;
  j["started_at"] = record.started_at;
  j["ended_at"] = record.ended_at;
  j["duration_seconds"] = record.duration_seconds;
  j["exit_code"] = record.exit_code;
  j["signal"] = record.signal;
  if (!record.launch_error.empty()) j["launch_error"] = record.launch_error;
  if (record.post_exit_code) j["post_exit_code"] = *record.post_exit_code;
  j["post_timed_out"] = record.post_timed_out;
  j["incomplete"] = record.incomplete;
  return j;
}

}  // namespace

Result<void> AppendSession(const std::filesystem::path& stats_file, const SessionRecord& record) {
  json whole = json::object();
  whole["session"] = json::array();

  std::error_code ec;
  if (std::filesystem::exists(stats_file, ec)) {
    toml::parse_result parsed = toml::parse_file(stats_file.string());
    if (!parsed) {
      const auto broken = stats_file.string() + ".bad";
      std::filesystem::rename(stats_file, broken, ec);
      log::Error("stats at {} could not be parsed ({}); kept it as {} and starting fresh",
                stats_file.string(), parsed.error().description(), broken);
    } else {
      const json existing = tomljson::ToJson(parsed.table());
      if (existing.contains("session") && existing["session"].is_array()) whole["session"] = existing["session"];
    }
  }

  whole["session"].push_back(SessionToJson(record));

  std::filesystem::create_directories(stats_file.parent_path(), ec);
  const auto temp = stats_file.string() + ".tmp";
  {
    std::ofstream out(temp);
    if (!out) return Err("stats_write_failed", std::format("cannot write {}", temp));
    out << tomljson::ToToml(whole);
  }
  std::filesystem::rename(temp, stats_file, ec);
  if (ec) return Err("stats_write_failed", ec.message());
  return {};
}

}  // namespace mira::proc
