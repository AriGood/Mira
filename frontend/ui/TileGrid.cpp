#include "TileGrid.h"

#include <QMouseEvent>
#include <QWheelEvent>

#include "GameTileDelegate.h"

namespace mira_gui {

TileGrid::TileGrid(QSize tile, QWidget* parent) : TileView(parent) {
  setItemDelegate(new GameTileDelegate(this, tile));
  setViewMode(QListView::IconMode);
  setResizeMode(QListView::Adjust);
  setMovement(QListView::Static);
  setUniformItemSizes(true);
  setSpacing(0);
  setGridSize(tile);
  setEditTriggers(QAbstractItemView::NoEditTriggers);
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  viewport()->setAutoFillBackground(false);
  setStyleSheet("QListWidget { background: transparent; border: none; }");
}

void TileGrid::SetTileSize(QSize tile) {
  static_cast<GameTileDelegate*>(itemDelegate())->SetTileSize(tile);
  setGridSize(tile);
  FitHeight();
}

int TileGrid::VisibleCount() const {
  int visible = 0;
  for (int row = 0; row < count(); ++row) {
    if (!item(row)->isHidden()) ++visible;
  }
  return visible;
}

void TileGrid::FitHeight() {
  const QSize cell = gridSize();
  const int per_row = qMax(1, viewport()->width() / qMax(1, cell.width()));
  const int rows = (VisibleCount() + per_row - 1) / per_row;
  setFixedHeight(rows * cell.height() + 2 * frameWidth());
}

void TileGrid::resizeEvent(QResizeEvent* event) {
  QListWidget::resizeEvent(event);
  FitHeight();
}

QListWidgetItem* TileGrid::ActionItemAt(const QPoint& pos) const {
  QListWidgetItem* hit = itemAt(pos);
  if (hit == nullptr || !hit->data(GameTileDelegate::ActionEnabledRole).toBool()) return nullptr;
  const QString text = hit->data(GameTileDelegate::ActionRole).toString();
  if (text.isEmpty()) return nullptr;
  return GameTileDelegate::ActionRect(visualItemRect(hit), text, font()).contains(pos) ? hit : nullptr;
}

void TileGrid::mouseMoveEvent(QMouseEvent* event) {
  viewport()->setCursor(ActionItemAt(event->pos()) != nullptr ? Qt::PointingHandCursor : Qt::ArrowCursor);
  TileView::mouseMoveEvent(event);
}

void TileGrid::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && on_action) {
    if (QListWidgetItem* hit = ActionItemAt(event->pos())) {
      on_action(hit);
      return;
    }
  }
  TileView::mouseReleaseEvent(event);
}

void TileGrid::wheelEvent(QWheelEvent* event) {
  StopHover();  // the tile is about to scroll out from under its card
  event->ignore();
}

}  // namespace mira_gui
