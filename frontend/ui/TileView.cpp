#include "TileView.h"

#include <QApplication>
#include <QItemSelection>
#include <QMouseEvent>
#include <QRubberBand>
#include <QScrollBar>
#include <QTimer>

#include <algorithm>

namespace mira_gui {

TileView::TileView(QWidget* parent) : QListWidget(parent) {
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setMouseTracking(true);
}

void TileView::SetDragSelectEnabled(bool enabled) {
  drag_select_enabled_ = enabled;
  if (!enabled) EndDrag();
}

void TileView::ForgetItems() {
  hover_.Forget();
  EndDrag();
}

void TileView::mousePressEvent(QMouseEvent* event) {
  // A plain click between or below the tiles deselects, like a file manager.
  if (event->button() == Qt::LeftButton && itemAt(event->pos()) == nullptr &&
      !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
    clearSelection();
    setCurrentItem(nullptr);
  }
  if (event->button() == Qt::LeftButton && drag_select_enabled_) {
    drag_origin_ = event->pos() + Offset();
    drag_modifiers_ = event->modifiers();
    tracking_drag_ = true;
  }
  QListWidget::mousePressEvent(event);
}

void TileView::mouseMoveEvent(QMouseEvent* event) {
  // The release went somewhere else (alt-tab, a desktop switch): drop the
  // drag instead of resuming it from the old origin.
  if (tracking_drag_ && !(event->buttons() & Qt::LeftButton)) EndDrag();
  if (event->buttons() & Qt::LeftButton) {
    if (tracking_drag_) {
      drag_pos_ = event->pos();
      UpdateDrag();
    }
    return;
  }
  hover_.Track(itemAt(event->pos()));
  QListWidget::mouseMoveEvent(event);
}

void TileView::mouseReleaseEvent(QMouseEvent* event) {
  const bool was_dragging = rubber_band_ != nullptr;
  EndDrag();
  if (was_dragging) return;  // the drag already applied the selection; not a click
  QListWidget::mouseReleaseEvent(event);
}

void TileView::leaveEvent(QEvent* event) {
  hover_.Track(nullptr);
  QListWidget::leaveEvent(event);
}

void TileView::focusOutEvent(QFocusEvent* event) {
  EndDrag();
  QListWidget::focusOutEvent(event);
}

void TileView::changeEvent(QEvent* event) {
  if (event->type() == QEvent::ActivationChange && !isActiveWindow()) EndDrag();
  QListWidget::changeEvent(event);
}

void TileView::wheelEvent(QWheelEvent* event) {
  hover_.Track(nullptr);
  QListWidget::wheelEvent(event);
  if (rubber_band_ != nullptr) UpdateDrag();
}

void TileView::UpdateDrag() {
  const QPoint current = drag_pos_ + Offset();
  if (rubber_band_ == nullptr) {
    // A click that wobbles a few pixels stays a click.
    if ((current - drag_origin_).manhattanLength() < QApplication::startDragDistance()) return;
    // A fresh drag replaces the selection unless it started with a
    // modifier held; either way, what's selected now is the floor a
    // shrinking rect won't clear again.
    if (!(drag_modifiers_ & (Qt::ControlModifier | Qt::ShiftModifier))) clearSelection();
    base_selection_.clear();
    for (QListWidgetItem* selected : selectedItems()) base_selection_.insert(selected);
    hover_.Track(nullptr);  // a dwell started before the drag would pop up mid-drag
    rubber_band_ = new QRubberBand(QRubberBand::Rectangle, viewport());
    rubber_band_->show();
    if (autoscroll_timer_ == nullptr) {
      autoscroll_timer_ = new QTimer(this);
      autoscroll_timer_->setInterval(16);
      connect(autoscroll_timer_, &QTimer::timeout, this, &TileView::AutoScrollStep);
    }
    autoscroll_timer_->start();
  }
  const QRect rect = QRect(drag_origin_, current).normalized();
  rubber_band_->setGeometry(rect.translated(-Offset()));
  // One select() call, not a setSelected per tile: each of those emits
  // its own itemSelectionChanged.
  QItemSelection selection;
  for (int row = 0; row < count(); ++row) {
    QListWidgetItem* it = item(row);
    if (it->isHidden()) continue;
    if (base_selection_.contains(it) || rect.intersects(visualItemRect(it).translated(Offset()))) {
      const QModelIndex index = indexFromItem(it);
      selection.select(index, index);
    }
  }
  selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
}

// Speed grows with how far into (or past) the edge band the cursor is.
void TileView::AutoScrollStep() {
  const int height = viewport()->height();
  int delta = 0;
  if (drag_pos_.y() < kAutoScrollEdge) {
    delta = drag_pos_.y() - kAutoScrollEdge;
  } else if (drag_pos_.y() > height - kAutoScrollEdge) {
    delta = drag_pos_.y() - (height - kAutoScrollEdge);
  }
  if (delta == 0) return;
  delta = std::clamp(delta / 2, -40, 40);
  if (delta == 0) delta = drag_pos_.y() < kAutoScrollEdge ? -1 : 1;
  QScrollBar* bar = verticalScrollBar();
  const int before = bar->value();
  bar->setValue(before + delta);
  if (bar->value() != before) UpdateDrag();
}

void TileView::EndDrag() {
  tracking_drag_ = false;
  if (autoscroll_timer_ != nullptr) autoscroll_timer_->stop();
  setState(QAbstractItemView::NoState);
  if (rubber_band_ == nullptr) return;
  // Deleted now, not deleteLater(): a second drag can start before a
  // deferred delete runs.
  delete rubber_band_;
  rubber_band_ = nullptr;
  base_selection_.clear();
}

}  // namespace mira_gui
