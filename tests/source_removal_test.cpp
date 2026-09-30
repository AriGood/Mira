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

TEST_CASE("DeleteInside leaves a folder above or beside a deeper root alone") {
  const fs::path root = test::TempDir("delete-inside-deep");
  test::Touch(root / "a" / "b" / "c" / "file");

  // The target is shorter than the root's path: nothing above the root may go.
  CHECK_FALSE(library::DeleteInside(root.string(), {root / "a" / "b" / "c"}));
  CHECK_FALSE(library::DeleteInside((root / "a").string(), {root / "a" / "b" / "c"}));
  CHECK(fs::exists(root / "a" / "b" / "c" / "file"));
}
