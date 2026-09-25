#include "library/ArchiveExtractor.h"

#include <fcntl.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <string>

#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

constexpr std::array kSuffixes = {std::string_view{".tar.gz"}, std::string_view{".tar.xz"},
                                  std::string_view{".tar.bz2"}, std::string_view{".tgz"},
                                  std::string_view{".tar"},     std::string_view{".zip"},
                                  std::string_view{".rar"},     std::string_view{".7z"},
                                  std::string_view{".7z.001"},  std::string_view{".zip.001"}};

// Game.part1.rar (new-style RAR volumes), Game.r00 (old-style, after Game.rar)
// and Game.7z.001 / Game.zip.001 (split files).
const std::regex kRarPart(R"(^(.*)\.part0*([0-9]+)\.rar$)", std::regex::icase);
const std::regex kRarOld(R"(^(.*)\.r[0-9]{2,3}$)", std::regex::icase);
const std::regex kSplit(R"(^(.*\.(?:7z|zip))\.([0-9]{3})$)", std::regex::icase);

// The longest matching suffix, or empty.
std::string_view SuffixOf(const fs::path& path) {
  const std::string name = strings::ToLower(path.filename().string());
  std::string_view best;
  for (std::string_view suffix : kSuffixes) {
    if (name.ends_with(suffix) && suffix.size() > best.size()) best = suffix;
  }
  return best;
}

std::string EscapeRegex(const std::string& text) {
  static const std::regex kSpecial(R"([.^$|()\[\]{}*+?\\])");
  return std::regex_replace(text, kSpecial, R"(\$&)");
}

// Siblings of `archive` whose name matches `pattern`, sorted by name.
std::vector<fs::path> SiblingsMatching(const fs::path& archive, const std::regex& pattern) {
  std::vector<fs::path> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(archive.parent_path(), ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.path() != archive && std::regex_match(name, pattern)) found.push_back(entry.path());
  }
  std::ranges::sort(found);
  return found;
}

// Moves everything in `from` into `to`, replacing entries with the same name.
Result<void> MoveContents(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  if (!fs::exists(to, ec)) {
    fs::rename(from, to, ec);
    if (ec) return Err("move_failed", ec.message());
    return {};
  }
  for (const auto& entry : fs::directory_iterator(from, ec)) {
    const fs::path target = to / entry.path().filename();
    fs::remove_all(target, ec);
    fs::rename(entry.path(), target, ec);
    if (ec) return Err("move_failed", std::format("{}: {}", target.string(), ec.message()));
  }
  fs::remove_all(from, ec);
  return {};
}

}  // namespace

bool LooksLikeArchive(const fs::path& path) { return !SuffixOf(path).empty() && !IsLaterVolume(path); }

bool IsLaterVolume(const fs::path& path) {
  const std::string name = path.filename().string();
  std::smatch match;
  if (std::regex_match(name, match, kRarPart)) return std::stoi(match[2]) != 1;
  if (std::regex_match(name, match, kSplit)) return match[2] != "001";
  return std::regex_match(name, kRarOld);
}

std::optional<fs::path> FirstVolumeOf(const fs::path& later) {
  const std::string name = later.filename().string();
  std::smatch match;
  std::regex first;
  if (std::regex_match(name, match, kRarPart)) {
    first = std::regex("^" + EscapeRegex(match[1]) + R"(\.part0*1\.rar$)", std::regex::icase);
  } else if (std::regex_match(name, match, kSplit)) {
    first = std::regex("^" + EscapeRegex(match[1]) + R"(\.001$)", std::regex::icase);
  } else if (std::regex_match(name, match, kRarOld)) {
    first = std::regex("^" + EscapeRegex(match[1]) + R"(\.rar$)", std::regex::icase);
  } else {
    return std::nullopt;
  }
  const std::vector<fs::path> found = SiblingsMatching(later, first);
  if (found.empty()) return std::nullopt;
  return found.front();
}

std::vector<fs::path> VolumesOf(const fs::path& archive) {
  const std::string name = archive.filename().string();
  std::smatch match;
  std::vector<fs::path> volumes = {archive};
  std::regex later;
  if (std::regex_match(name, match, kRarPart)) {
    later = std::regex("^" + EscapeRegex(match[1]) + R"(\.part[0-9]+\.rar$)", std::regex::icase);
  } else if (std::regex_match(name, match, kSplit)) {
    later = std::regex("^" + EscapeRegex(match[1]) + R"(\.[0-9]{3}$)", std::regex::icase);
  } else if (strings::ToLower(name).ends_with(".rar")) {
    later = std::regex("^" + EscapeRegex(name.substr(0, name.size() - 4)) + R"(\.r[0-9]{2,3}$)", std::regex::icase);
  } else {
    return volumes;
  }
  std::ranges::move(SiblingsMatching(archive, later), std::back_inserter(volumes));
  return volumes;
}

bool AnyOpenForWriting(const std::vector<fs::path>& paths) {
  std::set<std::string> wanted;
  for (const fs::path& path : paths) {
    std::error_code ec;
    const fs::path canonical = fs::canonical(path, ec);
    if (!ec) wanted.insert(canonical.string());
  }
  if (wanted.empty()) return false;

  std::error_code ec;
  for (const auto& proc : fs::directory_iterator("/proc", ec)) {
    const std::string pid = proc.path().filename().string();
    if (pid.empty() || !std::ranges::all_of(pid, ::isdigit)) continue;
    std::error_code fd_ec;
    for (const auto& fd : fs::directory_iterator(proc.path() / "fd", fd_ec)) {
      std::error_code link_ec;
      const fs::path target = fs::read_symlink(fd.path(), link_ec);
      if (link_ec || !wanted.contains(target.string())) continue;
      std::ifstream info(proc.path() / "fdinfo" / fd.path().filename());
      std::string key;
      std::string flags;
      while (info >> key >> flags) {
        if (key != "flags:") continue;
        if ((std::stoi(flags, nullptr, 8) & O_ACCMODE) != O_RDONLY) return true;
        break;
      }
    }
  }
  return false;
}

fs::path StemWithoutArchiveExtension(const fs::path& archive) {
  const std::string name = archive.filename().string();
  std::smatch match;
  if (std::regex_match(name, match, kRarPart)) return match[1].str();
  return name.substr(0, name.size() - SuffixOf(archive).size());
}

Result<void> ExtractAndRemove(const fs::path& archive, const fs::path& dest_dir) {
  const std::string_view suffix = SuffixOf(archive);
  if (suffix.empty() || IsLaterVolume(archive)) return Err("not_an_archive", "unrecognized archive extension");

  const fs::path work_dir = dest_dir.parent_path() / std::format("{}{}", kExtractingPrefix, dest_dir.filename().string());
  std::error_code ec;
  fs::remove_all(work_dir, ec);
  fs::create_directories(work_dir, ec);
  if (ec) return Err("dest_dir_failed", ec.message());

  const Result<void> extracted = runner::Extract(archive, work_dir);
  if (!extracted) {
    fs::remove_all(work_dir, ec);
    return extracted;
  }

  if (auto moved = MoveContents(work_dir, dest_dir); !moved) {
    fs::remove_all(work_dir, ec);
    return moved;
  }

  for (const fs::path& volume : VolumesOf(archive)) {
    fs::remove(volume, ec);
    if (ec) return Err("archive_cleanup_failed", ec.message());
  }
  return {};
}

}  // namespace mira::library
