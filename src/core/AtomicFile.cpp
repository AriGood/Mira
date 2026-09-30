#include "core/AtomicFile.h"

#include <format>
#include <fstream>

namespace mira {

Result<void> WriteFileAtomic(const std::filesystem::path& path, std::string_view content, std::string code) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  const auto temp = path.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::trunc);
    if (!out) return Err(code, std::format("couldn't write {}", temp), kDiskHint);
    out << content;
    out.flush();
    if (!out) {
      std::filesystem::remove(temp, ec);
      return Err(code, std::format("couldn't write {}", temp), kDiskHint);
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) return Err(code, std::format("couldn't save {}: {}", path.string(), ec.message()), kDiskHint);
  return {};
}

}  // namespace mira
