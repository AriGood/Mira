#pragma once

#include <QColor>
#include <QDateTime>
#include <QString>

#include "Theme.h"

#include <cstdint>
#include <optional>
#include <string>

// Small presentation helpers shared between MainWindow's table and
// GameDetailDialog, so a game reads the same way in both places.
namespace mira_gui {

// A color per lifecycle state so status reads at a glance without a
// legend. The theme owns the colors; see ui/Theme.h.
inline QColor StatusColor(const std::string& status) {
  const theme::Tokens& tokens = theme::Current();
  if (status == "ready") return tokens.status_ready;
  if (status == "setting_up") return tokens.status_setting_up;
  if (status == "broken") return tokens.status_broken;
  if (status == "missing") return tokens.status_missing;
  if (status == "needs_install") return tokens.status_needs_install;
  return tokens.text_muted;
}

// ProtonDB's own tier colors, read out of their production site's own
// bundle (head.protondb.pages.dev/static/js/main.*.js, theme.colors.medals)
// rather than approximated. "native" is deliberately not part of that
// `medals` map on their side either — it lives as its own top-level
// `native: "green"` entry in the same object, because it means "doesn't
// touch Proton at all" rather than grading how well Proton runs it, which
// is the distinction ProtonDbTierIsNative below exists to carry into ours.
inline QColor ProtonDbTierColor(const std::string& tier) {
  if (tier == "platinum") return QColor("#b4c7dc");
  if (tier == "gold") return QColor("#cfb53b");
  if (tier == "silver") return QColor("#a6a6a6");
  if (tier == "bronze") return QColor("#cd7f32");
  if (tier == "borked" || tier == "garbage") return QColor("#ff0000");
  if (tier == "native") return QColor("#008000");
  return QColor("#444444");  // "pending"
}

inline bool ProtonDbTierIsNative(const std::string& tier) { return tier == "native"; }

// Platinum/gold/silver are light enough that white text on them would wash
// out; the rest are dark enough that black text would.
inline QColor ContrastingTextColor(const QColor& background) {
  const double luminance =
      0.299 * background.red() + 0.587 * background.green() + 0.114 * background.blue();
  return luminance > 140 ? QColor("#1c1f25") : QColor("#ffffff");
}

// Shared by both library views so the same game reads identically in the
// grid, its details panel, and the classic table.
inline QString StatusLabel(const std::string& status) {
  if (status == "needs_install") return "Needs install";
  if (status == "setting_up") return "Setting up";
  QString label = QString::fromStdString(status);
  if (!label.isEmpty()) label[0] = label[0].toUpper();
  return label;
}

inline QString FormatLastPlayed(const std::optional<std::int64_t>& last_played_at) {
  if (!last_played_at) return "Never";
  return QDateTime::fromSecsSinceEpoch(*last_played_at).toString("yyyy-MM-dd HH:mm");
}

inline QString FormatPlaytime(std::int64_t play_seconds) {
  if (play_seconds <= 0) return "—";
  const std::int64_t hours = play_seconds / 3600;
  const std::int64_t minutes = (play_seconds % 3600) / 60;
  if (hours > 0) return QString("%1h %2m").arg(hours).arg(minutes);
  if (minutes > 0) return QString("%1m").arg(minutes);
  return "<1m";
}

}  // namespace mira_gui
