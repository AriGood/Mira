#include "library/Detector.h"

#include <sys/stat.h>

#include <algorithm>
#include <fstream>

#include "core/Strings.h"

namespace mira::library {
namespace {

namespace fs = std::filesystem;

struct RawCandidate {
  fs::path rel_path;  // relative to the game folder
  model::Platform kind;
  int depth = 0;
};

bool MatchesAny(const std::vector<std::string>& globs, std::string_view text) {
  return std::ranges::any_of(globs, [&](const std::string& glob) {
    return strings::GlobMatch(strings::ToLower(glob), strings::ToLower(text));
  });
}

// True for a regular file whose first four bytes are the ELF magic number —
// used instead of relying solely on the executable bit, which extraction
// tools frequently drop or set inconsistently.
bool LooksLikeElf(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  char magic[4] = {};
  file.read(magic, sizeof(magic));
  return file.gcount() == sizeof(magic) && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' &&
        magic[3] == 'F';
}

bool HasExecuteBit(const fs::path& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR) != 0;
}

// Walks `folder` up to max_depth, skipping anything matching ignore_globs
// (relative to `folder`, so "*/prefix/*" excludes a prefix directory the
// caller nested inside the game folder, and a bare glob like ".*" excludes
// dotfiles at any depth).
std::vector<RawCandidate> WalkForExecutables(const fs::path& folder, const DetectorSettings& settings) {
  std::vector<RawCandidate> found;

  const std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
    if (depth > settings.max_depth) return;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
      const fs::path rel = fs::relative(entry.path(), folder, ec);
      if (ec) continue;
      if (MatchesAny(settings.ignore_globs, rel.generic_string())) continue;

      if (entry.is_directory(ec)) {
        walk(entry.path(), depth + 1);
        continue;
      }
      if (!entry.is_regular_file(ec)) continue;

      const std::string ext = strings::ToLower(entry.path().extension().string());
      if (ext == ".exe") {
        found.push_back({rel, model::Platform::Windows, depth});
      } else if (ext == ".sh" || HasExecuteBit(entry.path()) || LooksLikeElf(entry.path())) {
        // A .sh is a candidate regardless of its executable bit (archives
        // routinely lose it); anything else needs the bit or ELF magic so a
        // stray data file doesn't get treated as a launcher.
        if (ext != ".exe") found.push_back({rel, model::Platform::Native, depth});
      }
    }
  };
  walk(folder, 0);
  return found;
}

double NameSimilarityBonus(const RawCandidate& candidate, const std::string& cleaned_folder_name,
                           double bonus) {
  const std::string stem = fs::path(candidate.rel_path).stem().string();
  return bonus * strings::Similarity(stem, cleaned_folder_name);
}

}  // namespace

Detector::Result Detector::Detect(const fs::path& folder) const {
  const std::string cleaned_name = strings::CleanGameName(folder.filename().string());
  std::vector<RawCandidate> raw = WalkForExecutables(folder, settings_);

  std::vector<double> scores(raw.size(), 0.0);
  const auto has_rule = [&](std::string_view name) {
    return std::ranges::find(settings_.rules, name) != settings_.rules.end();
  };

  if (has_rule("deny_patterns")) {
    for (size_t i = 0; i < raw.size(); ++i) {
      const std::string basename = raw[i].rel_path.filename().string();
      if (MatchesAny(settings_.deny_name_patterns, basename)) scores[i] -= 10.0;
    }
  }
  if (has_rule("name_similarity")) {
    for (size_t i = 0; i < raw.size(); ++i) {
      scores[i] += NameSimilarityBonus(raw[i], cleaned_name, settings_.name_match_bonus);
    }
  }
  if (has_rule("depth")) {
    for (size_t i = 0; i < raw.size(); ++i) scores[i] -= settings_.depth_penalty * raw[i].depth;
  }
  if (has_rule("shallowest") && !raw.empty()) {
    const int min_depth = std::ranges::min(raw, {}, &RawCandidate::depth).depth;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i].depth == min_depth) scores[i] += 1.0;
    }
  }

  std::vector<size_t> order(raw.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::ranges::sort(order, [&](size_t a, size_t b) { return scores[a] > scores[b]; });

  Result result;
  result.candidates.reserve(raw.size());
  for (size_t rank = 0; rank < order.size(); ++rank) {
    const size_t i = order[rank];
    result.candidates.push_back({
        .rel_path = raw[i].rel_path.generic_string(),
        .kind = raw[i].kind,
        .score = scores[i],
        .chosen = rank == 0,
    });
  }

  // Confidence: how much to trust the top pick. A clear score margin over
  // the runner-up, and a close name match to the folder, both raise it; a
  // single unopposed candidate gets a moderate baseline rather than full
  // confidence, since "the only thing found" is not the same guarantee as
  // "clearly the right one". These weights are deliberately simple — they
  // are meant to be retuned via detect.* config, not treated as final.
  if (result.candidates.empty()) {
    result.confidence = 0.0;
  } else {
    const double name_component = 0.3 * strings::Similarity(
        fs::path(result.candidates[0].rel_path).stem().string(), cleaned_name);
    double margin_component;
    if (result.candidates.size() == 1) {
      margin_component = 0.4;
    } else {
      const double margin = result.candidates[0].score - result.candidates[1].score;
      margin_component = std::clamp(margin / 5.0, 0.0, 0.4);
    }
    result.confidence = std::clamp(0.3 + margin_component + name_component, 0.0, 1.0);
  }

  return result;
}

}  // namespace mira::library
