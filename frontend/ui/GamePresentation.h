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
