#include "config/Config.h"

#include <format>
#include <fstream>
#include <sstream>

#include "config/Schema.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/TomlJson.h"

namespace mira::config {
namespace {
using nlohmann::json;
}

Config::Config(std::filesystem::path file) : file_(std::move(file)) {
  document_ = Schema::Instance().Defaults();
}

void Config::Load() {
  const Schema& schema = Schema::Instance();
  std::lock_guard lock(mutex_);
  document_ = schema.Defaults();
  frontend_ = json::object();

  std::error_code ec;
  const auto write_defaults = [&] {
    std::filesystem::create_directories(file_.parent_path(), ec);
    std::ofstream out(file_);
    if (out) out << tomljson::ToToml(document_);
  };

  if (!std::filesystem::exists(file_, ec)) {
    log::Info("no settings at {}, writing defaults", file_.string());
    write_defaults();
    return;
  }

  toml::parse_result parsed = toml::parse_file(file_.string());
  if (!parsed) {
    const auto broken = file_.string() + ".bad";
    std::filesystem::rename(file_, broken, ec);
    log::Error("settings at {} could not be parsed ({}); kept it as {} and continuing with "
              "defaults",
              file_.string(), parsed.error().description(), broken);
    write_defaults();
    return;
  }

  json whole = tomljson::ToJson(parsed.table());
  if (whole.contains("frontend") && whole["frontend"].is_object()) {
    frontend_ = whole["frontend"];
  }
  whole.erase("frontend");

  // An individual bad value falls back to its default rather than rejecting
  // the whole file, so one typo cannot leave the user with a daemon that
  // refuses to start.
  for (const std::string& problem : schema.ValidateDocument(whole)) {
    log::Warn("settings: {} (using the default)", problem);
  }
  for (const Entry& entry : schema.Entries()) {
    const auto pointer = Schema::Pointer(entry.key);
    if (!whole.contains(pointer)) continue;
    if (schema.Validate(entry.key, whole[pointer])) whole[pointer] = entry.default_value;
  }

  document_.merge_patch(whole);
}

Result<void> Config::Save() {
  std::lock_guard lock(mutex_);
  std::error_code ec;
  std::filesystem::create_directories(file_.parent_path(), ec);

  json whole = document_;
  whole["frontend"] = frontend_;

  // Write-and-rename so an interrupted save cannot truncate a working file.
  const auto temp = file_.string() + ".tmp";
  {
    std::ofstream out(temp);
    if (!out) return Err("config_write_failed", std::format("cannot write {}", temp));
    out << tomljson::ToToml(whole);
  }
  std::filesystem::rename(temp, file_, ec);
  if (ec) return Err("config_write_failed", ec.message());
  return {};
}

json Config::Document() const {
  std::lock_guard lock(mutex_);
  return document_;
}

json Config::GetLocked(std::string_view key) const {
  const auto pointer = Schema::Pointer(key);
  if (document_.contains(pointer)) return document_[pointer];
  if (const Entry* entry = Schema::Instance().Find(key)) return entry->default_value;
  return json();
}

json Config::Get(std::string_view key) const {
  std::lock_guard lock(mutex_);
  return GetLocked(key);
}

Result<void> Config::Set(std::string_view key, const json& value) {
  if (auto problem = Schema::Instance().Validate(key, value)) {
    return Err("invalid_setting", std::format("{}: {}", key, *problem));
  }
  {
    std::lock_guard lock(mutex_);
    document_[Schema::Pointer(key)] = value;
  }
  return Save();
}

Result<void> Config::Patch(const json& patch) {
  const Schema& schema = Schema::Instance();
  if (!patch.is_object()) return Err("invalid_patch", "expected a JSON object");

  // Validate the whole patch first: a rejected patch must change nothing.
  std::vector<std::string> problems;
  for (const Entry& entry : schema.Entries()) {
    const auto pointer = Schema::Pointer(entry.key);
    if (!patch.contains(pointer)) continue;
    if (auto problem = schema.Validate(entry.key, patch[pointer])) {
      problems.push_back(std::format("{}: {}", entry.key, *problem));
    }
  }
  if (!problems.empty()) {
    std::string joined;
    for (const std::string& problem : problems) {
      if (!joined.empty()) joined += "; ";
      joined += problem;
    }
    return Err("invalid_setting", joined);
  }

  {
    std::lock_guard lock(mutex_);
    document_.merge_patch(patch);
    if (patch.contains("frontend") && patch["frontend"].is_object()) {
      frontend_.merge_patch(patch["frontend"]);
    }
  }
  return Save();
}

Result<void> Config::Reset(std::string_view key) {
  const Entry* entry = Schema::Instance().Find(key);
  if (entry == nullptr) return Err("unknown_setting", std::format("unknown setting \"{}\"", key));
  {
    std::lock_guard lock(mutex_);
    document_[Schema::Pointer(key)] = entry->default_value;
  }
  return Save();
}

void Config::ResetAll() {
  std::lock_guard lock(mutex_);
  document_ = Schema::Instance().Defaults();
}

json Config::FrontendSettings() const {
  std::lock_guard lock(mutex_);
  return frontend_;
}

void Config::SetFrontendSettings(json settings) {
  {
    std::lock_guard lock(mutex_);
    frontend_ = std::move(settings);
  }
  if (auto result = Save(); !result) {
    log::Error("failed to save frontend settings: {}", result.error().message);
  }
}

bool Config::GetBool(std::string_view key) const {
  const json value = Get(key);
  return value.is_boolean() && value.get<bool>();
}

std::int64_t Config::GetInt(std::string_view key) const {
  const json value = Get(key);
  return value.is_number() ? value.get<std::int64_t>() : 0;
}

double Config::GetDouble(std::string_view key) const {
  const json value = Get(key);
  return value.is_number() ? value.get<double>() : 0.0;
}

std::string Config::GetString(std::string_view key) const {
  const json value = Get(key);
  return value.is_string() ? value.get<std::string>() : std::string();
}

std::vector<std::string> Config::GetStringArray(std::string_view key) const {
  std::vector<std::string> out;
  const json value = Get(key);
  if (!value.is_array()) return out;
  for (const json& item : value) {
    if (item.is_string()) out.push_back(item.get<std::string>());
  }
  return out;
}

std::filesystem::path Config::GetPath(std::string_view key) const {
  return paths::Expand(GetString(key));
}

std::vector<std::filesystem::path> Config::GetPathArray(std::string_view key) const {
  std::vector<std::filesystem::path> out;
  for (const std::string& value : GetStringArray(key)) out.push_back(paths::Expand(value));
  return out;
}

}  // namespace mira::config
