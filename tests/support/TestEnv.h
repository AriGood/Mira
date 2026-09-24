#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "api/EventBus.h"
#include "config/Config.h"
#include "store/GameStore.h"

// Shared test setup. New tests should start from TestEnv rather than
// building their own config and store.
namespace mira::test {

// A fresh, empty directory under $TMPDIR/mira-tests.
std::filesystem::path TempDir(std::string_view name);

// Creates `path` (and its parents) with `content`.
void Touch(const std::filesystem::path& path, std::string_view content = "", bool executable = false);

// A temp state directory with a loaded Config, GameStore and EventBus.
// Defaults keep tests fast and offline: prefixes live in the temp dir,
// Windows games provision with native:native (no real Proton), and no
// metadata is fetched.
struct TestEnv {
  explicit TestEnv(std::string_view name);

  std::filesystem::path dir;
  config::Config config;
  store::GameStore games;
  api::EventBus events;
};

// One real Proton prefix for every test that needs actual Wine, made on
// first use and kept between runs. Empty when no Proton build is
// installed, so a test can skip.
std::filesystem::path SharedProtonPrefix();

}  // namespace mira::test
