#include <doctest.h>

#include "library/SourceRemoval.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("DeleteInside only deletes strictly inside a root") {
  const fs::path root = test::TempDir("delete-inside");
  test::Touch(root / "game" / "game.exe");
  test::Touch(root.parent_path() / "delete-inside-outside" / "keep");

  CHECK_FALSE(library::DeleteInside(root.string(), {root}));
  CHECK(fs::exists(root));
  CHECK_FALSE(
      library::DeleteInside((root.parent_path() / "delete-inside-outside").string(), {root}));
  CHECK(fs::exists(root.parent_path() / "delete-inside-outside" / "keep"));
  CHECK(library::DeleteInside((root / "game").string(), {root}));
  CHECK_FALSE(fs::exists(root / "game"));
}
