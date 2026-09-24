#pragma once

#include <QSize>
#include <QStyledItemDelegate>

namespace mira_gui {

// Paints one cover tile in the library grid: the artwork, a scrim, the title
// over it, and the status.
//
// Everything it draws comes from the item's own roles, so a repaint never
// reaches back into the window that owns the grid — the grid can be
// rebuilt from scratch on every keystroke without the painting code
// knowing that happened.
class GameTileDelegate : public QStyledItemDelegate {
public:
  // The roles the grid sets on each item and this delegate reads.
  enum Role {
    IdRole = Qt::UserRole + 1,
    NameRole,
    StatusRole,
    RunningRole,
    // Optional: a button-like pill painted in the tile's top-right corner
    // ("Install", "Installing…"). Hit-test clicks with ActionRect.
    ActionRole,
    ActionEnabledRole,
  };

  // Where the ActionRole pill sits inside a tile's cell.
  static QRect ActionRect(const QRect& cell, const QString& text, const QFont& font);

  GameTileDelegate(QObject* parent, QSize tile);

  void SetTileSize(QSize tile);

  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override;

private:
  QSize tile_;
};

}  // namespace mira_gui
