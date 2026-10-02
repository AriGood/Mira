#include "proc/Stats.h"

#include <mutex>
#include <format>
#include <fstream>

#include <toml.hpp>

#include "core/AtomicFile.h"
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
  // Two games ending together would otherwise both read the old file and the last write would win.
  static std::mutex mutex;
  const std::lock_guard lock(mutex);
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

  return WriteFileAtomic(stats_file, tomljson::ToTomlText(whole), "stats_write_failed");
}

}  // namespace mira::proc
