#include "support/TestEnv.h"

#include <fstream>

#include "model/Types.h"
#include "runner/RunnerRegistry.h"

namespace mira::test {
namespace fs = std::filesystem;

fs::path TempDir(std::string_view name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void Touch(const fs::path& path, std::string_view content, bool executable) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << content;
  if (executable) fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
}

TestEnv::TestEnv(std::string_view name)
    : dir(TempDir(name)), config(dir / "settings.toml"), games(dir / "games.toml") {
  config.Load();
  games.Load();
  [[maybe_unused]] auto a = config.Set("prefix_root", (dir / "prefixes").string());
  [[maybe_unused]] auto b = config.Set("default_runner.windows", "native:native");
  [[maybe_unused]] auto c = config.Set("metadata.enabled", false);
}

fs::path SharedProtonPrefix() {
  static const fs::path prefix = [] {
    const fs::path root = fs::temp_directory_path() / "mira-tests-shared";
    const fs::path data_dir = root / "proton-prefix";
    std::error_code ec;
    if (fs::exists(data_dir / "drive_c", ec)) return data_dir;

    config::Config config(root / "settings.toml");
    config.Load();
    [[maybe_unused]] auto set = config.Set("default_runner.windows", "proton:latest");
    const runner::RunnerRegistry runners(config);
    model::Game game;
    game.id = "shared-test-prefix";
    game.platform = model::Platform::Windows;
    game.data_dir = data_dir.string();
    const model::Game provisioned = runners.ProvisionGame(game);
    return provisioned.status == model::GameStatus::Ready ? data_dir : fs::path();
  }();
  return prefix;
}

}  // namespace mira::test
