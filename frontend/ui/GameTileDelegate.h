#pragma once

#include <QSize>
#include <QStyledItemDelegate>

namespace mira_gui {

// Paints one cover tile in the library grid: the artwork, a scrim, the title
// over it, and the status.
//
// Everything it draws comes from the item's own roles, so a repaint never
// reaches back into the window that owns the grid — which is what lets the
// grid be rebuilt from scratch on every keystroke in the search box without
// the painting code having to know that happened.
class GameTileDelegate : public QStyledItemDelegate {
public:
  // The roles the grid sets on each item and this delegate reads.
  enum Role {
    IdRole = Qt::UserRole + 1,
    NameRole,
    StatusRole,
    RunningRole,
  };

  GameTileDelegate(QObject* parent, QSize tile);

  void SetTileSize(QSize tile);

  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override;

private:
  QSize tile_;
};

}  // namespace mira_gui
