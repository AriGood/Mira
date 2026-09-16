#include "library/WinePrefix.h"

namespace mira::library {

bool LooksLikeWinePrefix(const std::filesystem::path& dir) {
  std::error_code ec;
  const bool has_registry = std::filesystem::exists(dir / "system.reg", ec);
  const bool has_drive_c = std::filesystem::exists(dir / "drive_c", ec);
  const bool has_pfx = std::filesystem::exists(dir / "pfx", ec);
  return (has_registry && has_drive_c) || has_pfx;
}

}  // namespace mira::library
