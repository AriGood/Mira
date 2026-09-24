#pragma once

#include <QListWidget>
#include <QSize>

#include <functional>

#include "HoverCard.h"

namespace mira_gui {

class GameTileDelegate;

// A wrapping grid of cover tiles that grows to fit them instead of
// scrolling, for a page that scrolls as a whole. Tiles are painted by
// GameTileDelegate; set its roles on each item.
class TileGrid : public QListWidget {
public:
  TileGrid(QSize tile, QWidget* parent = nullptr);

  // A tile's ActionRole pill was clicked (only while ActionEnabledRole).
  std::function<void(QListWidgetItem*)> on_action;
  // An item after the cursor rests on it, nullptr once it moves off.
  std::function<void(QListWidgetItem*)> on_hover_item;

  // Call after adding, removing or hiding items.
  void FitHeight();
  // Items not hidden.
  int VisibleCount() const;
  // Before clear(): a pending hover could still hold an item.
  void ForgetItems() { hover_.Forget(); }

protected:
  void resizeEvent(QResizeEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  // Left to the page's own scroll area.
  void wheelEvent(QWheelEvent* event) override;

private:
  QListWidgetItem* ActionItemAt(const QPoint& pos) const;

  HoverDwell hover_{[this](QListWidgetItem* item) {
    if (on_hover_item) on_hover_item(item);
  }};
};

}  // namespace mira_gui
