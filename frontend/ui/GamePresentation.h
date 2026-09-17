#pragma once

#include <QColor>
#include <QDateTime>
#include <QString>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

// Small presentation helpers shared between MainWindow's table and
// GameDetailDialog, so a game reads the same way in both places.
namespace mira_gui {

// A soft color per lifecycle state (docs/api.md: setting_up | ready | broken
// | missing | needs_install) so status reads at a glance without a legend.
inline QColor StatusColor(const std::string& status) {
  if (status == "ready") return QColor("#2e7d32");
  if (status == "setting_up") return QColor("#1565c0");
  if (status == "broken") return QColor("#c62828");
  if (status == "missing") return QColor("#757575");
  if (status == "needs_install") return QColor("#ef6c00");
  return QColor("#424242");
}

// `reviewed` and `confidence` are independent (docs/api.md): a human hasn't
// necessarily looked at a game just because the detector is sure of it. A
// reviewed game is trustworthy regardless of what the detector originally
// scored, so it always reads green; an unreviewed one is colored by how much
// to trust the auto-detection, red (low) through yellow to green (high).
inline QString ConfidenceText(bool reviewed, double confidence) {
  const QString percent = QString("%1%").arg(qRound(confidence * 100));
  return reviewed ? QString("✓ %1").arg(percent) : percent;
}

inline QColor ConfidenceColor(bool reviewed, double confidence) {
  if (reviewed) return QColor("#2e7d32");
  const double clamped = std::clamp(confidence, 0.0, 1.0);
  const int hue = qRound(clamped * 120.0);  // 0 = red, 120 = green (HSV wheel)
  return QColor::fromHsv(hue, 200, 170);
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
