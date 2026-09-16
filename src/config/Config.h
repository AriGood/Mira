#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

#include "core/Result.h"

namespace mira::config {

// The on-disk settings.toml, kept merged with the schema defaults so every
// declared key always resolves. In memory everything is JSON (the same
// representation used over the API); only Load()/Save() speak TOML, via
// core/TomlJson. Unknown backend keys are preserved across writes, so a newer
// frontend/daemon's settings survive an older one rather than being dropped.
//
// One TOML table, [frontend], is reserved for the frontend's own settings.
// The backend does not know its shape and never validates it — it is stored
// and returned verbatim so the two processes can share one settings file
// without the daemon needing to track the frontend's schema.
class Config {
public:
  explicit Config(std::filesystem::path file);

  // Never fails: a missing file is created from defaults and an unparseable
  // one is preserved as <file>.bad and replaced. Priority one is that the
  // daemon starts and works, so a broken config degrades rather than blocking.
  void Load();
  Result<void> Save();

  nlohmann::json Document() const;  // backend keys only
  nlohmann::json Get(std::string_view key) const;

  Result<void> Set(std::string_view key, const nlohmann::json& value);
  // Applies a partial document, validating every key before changing anything.
  Result<void> Patch(const nlohmann::json& patch);
  Result<void> Reset(std::string_view key);
  void ResetAll();

  // The opaque [frontend] table, read and written without validation.
  nlohmann::json FrontendSettings() const;
  void SetFrontendSettings(nlohmann::json settings);

  bool GetBool(std::string_view key) const;
  std::int64_t GetInt(std::string_view key) const;
  double GetDouble(std::string_view key) const;
  std::string GetString(std::string_view key) const;
  std::vector<std::string> GetStringArray(std::string_view key) const;

  // String value with "~" and $VAR expanded.
  std::filesystem::path GetPath(std::string_view key) const;
  std::vector<std::filesystem::path> GetPathArray(std::string_view key) const;

  const std::filesystem::path& File() const { return file_; }

private:
  nlohmann::json GetLocked(std::string_view key) const;

  mutable std::mutex mutex_;
  std::filesystem::path file_;
  nlohmann::json document_;
  nlohmann::json frontend_ = nlohmann::json::object();
};

}  // namespace mira::config
