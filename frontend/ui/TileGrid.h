#pragma once

#include <QListWidget>
#include <QSize>

#include <functional>

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

  // Call after adding, removing or hiding items.
  void FitHeight();
  // Items not hidden.
  int VisibleCount() const;

protected:
  void resizeEvent(QResizeEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  // Left to the page's own scroll area.
  void wheelEvent(QWheelEvent* event) override;

private:
  QListWidgetItem* ActionItemAt(const QPoint& pos) const;
};

}  // namespace mira_gui
