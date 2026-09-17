#pragma once

#include <QColor>
#include <QPixmap>
#include <QSize>
#include <QString>

namespace mira_gui {

// Placeholder cover art.
//
// The backend has no artwork of its own yet (it's a planned change — see
// docs/api.md), but a grid of identical grey rectangles is unusable, so
// every game gets a generated cover instead of a blank one. It is derived
// from the game's id, which means it is stable across restarts, across
// renames, and across every machine showing the same library — a user
// learns "the teal one with WS" as that game's tile and it stays that way.
// When real artwork lands, this stays as the fallback for games that have
// none.

QColor PlaceholderBase(const QString& seed);

// Up to two initials, from the first two words that start with a letter or
// digit. "The Witcher 3" reads better as "TW" than as "T3", so the numeric
// tail of a title is only used when there is nothing else.
QString CoverInitials(const QString& name);

QPixmap PlaceholderCover(const QString& name, const QString& seed, QSize size, qreal dpr);

}  // namespace mira_gui
