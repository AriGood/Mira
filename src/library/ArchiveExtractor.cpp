#include "library/ArchiveExtractor.h"

#include <array>
#include <format>

#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

struct ArchiveKind {
  std::string_view suffix;
  std::string_view tool;     // binary that must be on PATH
  std::string_view package;  // what to tell the user to install if it's missing
};

constexpr std::array kKinds = {
    ArchiveKind{".tar.gz", "tar", "tar"},  ArchiveKind{".tar.xz", "tar", "tar"},
    ArchiveKind{".tar.bz2", "tar", "tar"}, ArchiveKind{".tgz", "tar", "tar"},
    ArchiveKind{".tar", "tar", "tar"},     ArchiveKind{".zip", "unzip", "unzip"},
    ArchiveKind{".rar", "unrar", "unrar"}, ArchiveKind{".7z", "7z", "p7zip"},
};

// Longest matching suffix wins, so ".tar.gz" is picked over a hypothetical
// bare ".gz" entry rather than whichever happens to come first in kKinds.
const ArchiveKind* KindOf(const fs::path& path) {
  const std::string name = strings::ToLower(path.filename().string());
  const ArchiveKind* best = nullptr;
  for (const ArchiveKind& kind : kKinds) {
    if (name.ends_with(kind.suffix) && (best == nullptr || kind.suffix.size() > best->suffix.size())) {
      best = &kind;
    }
  }
  return best;
}

}  // namespace

bool LooksLikeArchive(const fs::path& path) { return KindOf(path) != nullptr; }

fs::path StemWithoutArchiveExtension(const fs::path& archive) {
  const ArchiveKind* kind = KindOf(archive);
  const std::string name = archive.filename().string();
  if (kind == nullptr) return name;
  return name.substr(0, name.size() - kind->suffix.size());
}

Result<void> ExtractAndRemove(const fs::path& archive, const fs::path& dest_dir) {
  const ArchiveKind* kind = KindOf(archive);
  if (kind == nullptr) return Err("not_an_archive", "unrecognized archive extension");

  if (!runner::FindOnPath(kind->tool)) {
    return Err("extractor_missing",
              std::format("install {} to auto-extract {} archives", kind->package, kind->suffix));
  }

  std::error_code ec;
  fs::create_directories(dest_dir, ec);
  if (ec) return Err("dest_dir_failed", ec.message());

  Command command;
  if (kind->tool == "tar") {
    command.argv = {"tar", "-xf", archive.string(), "-C", dest_dir.string()};
  } else if (kind->tool == "unzip") {
    command.argv = {"unzip", "-q", archive.string(), "-d", dest_dir.string()};
  } else if (kind->tool == "unrar") {
    command.argv = {"unrar", "x", "-o+", archive.string(), dest_dir.string() + "/"};
  } else if (kind->tool == "7z") {
    command.argv = {"7z", "x", archive.string(), std::format("-o{}", dest_dir.string()), "-y"};
  }

  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) {
    return Err("extract_failed", !result ? result.error().message
                                        : std::format("{} exited {}: {}", kind->tool, result->exit_code,
                                                      result->output));
  }

  fs::remove(archive, ec);
  if (ec) return Err("archive_cleanup_failed", ec.message());
  return {};
}

}  // namespace mira::library
