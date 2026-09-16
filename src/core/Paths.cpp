#include "core/Paths.h"

#include <pwd.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>

namespace mira::paths {
namespace {

std::filesystem::path EnvOr(const char* name, const std::filesystem::path& fallback) {
  const char* value = std::getenv(name);
  return (value && *value) ? std::filesystem::path(value) : fallback;
}

}  // namespace

std::filesystem::path Home() {
  if (const char* home = std::getenv("HOME"); home && *home) return home;
  if (const passwd* pw = ::getpwuid(::getuid())) return pw->pw_dir;
  return "/tmp";
}

std::filesystem::path UserDir() { return EnvOr("XDG_CONFIG_HOME", Home() / ".config") / "mira"; }

std::filesystem::path RuntimeDir() {
  // XDG_RUNTIME_DIR is absent under some session managers; /tmp keeps the
  // daemon startable rather than failing over a socket location.
  return EnvOr("XDG_RUNTIME_DIR", std::filesystem::path("/tmp")) / "mira";
}

std::filesystem::path SettingsFile() { return UserDir() / "settings.toml"; }
std::filesystem::path GamesFile() { return UserDir() / "games.toml"; }
std::filesystem::path DefaultSocket() { return RuntimeDir() / "mirad.sock"; }

std::filesystem::path Expand(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());

  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '~' && i == 0 && (raw.size() == 1 || raw[1] == '/')) {
      out += Home().string();
    } else if (raw[i] == '$' && i + 1 < raw.size()) {
      size_t end = i + 1;
      while (end < raw.size() &&
             (std::isalnum(static_cast<unsigned char>(raw[end])) != 0 || raw[end] == '_')) {
        ++end;
      }
      const std::string name(raw.substr(i + 1, end - i - 1));
      if (const char* value = std::getenv(name.c_str())) out += value;
      i = end - 1;
    } else {
      out += raw[i];
    }
  }
  return out;
}

}  // namespace mira::paths
