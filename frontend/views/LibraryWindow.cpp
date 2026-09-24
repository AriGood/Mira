#include "LibraryWindow.h"
#include <algorithm>

#include <QAbstractItemView>
#include <QAction>
#include <QButtonGroup>
#include <QKeySequence>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QListWidgetItem>
#include <QMenu>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QGridLayout>
#include <QGuiApplication>
#include <QPushButton>
#include <QRubberBand>
#include <QScreen>
#include <QScrollBar>
#include <QItemSelection>
#include <QScrollArea>
#include <QSet>
#include <QSlider>
#include <QSplitter>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QHeaderView>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <functional>
#include <iterator>
#include <optional>

#include "../client/MiradClient.h"
#include "../dialogs/AddManualGameDialog.h"
#include "../dialogs/ArtworkPickerDialog.h"
#include "../dialogs/DesktopEntryImportDialog.h"
#include "../dialogs/GameDetailDialog.h"
#include "../dialogs/GameDetailPageDialog.h"
#include "../dialogs/RunnerDialog.h"

#include "../ui/AboutPanel.h"
#include "../ui/CoverArt.h"
#include "../ui/GameActions.h"
#include "../ui/GameEditForm.h"
#include "../ui/GamePresentation.h"
#include "../ui/GameTileDelegate.h"
#include "../ui/HoverCard.h"
#include "../ui/Icons.h"
#include "../ui/KeyBindings.h"
#include "../ui/LibrarySort.h"
#include "../ui/Notify.h"
#include "../ui/SettingsPanel.h"
#include "../ui/Shortcuts.h"
#include "../ui/Theme.h"
#include "../ui/Tray.h"
#include "SourcePage.h"

// setViewportMargins is protected on QAbstractScrollArea; this just republishes
// it so ApplyLayoutTokens() can pad the tiles without also inseting the
// scrollbar (a container's own contents margins would do both).
//
// Also implements its own drag-to-select: every tile fills its whole grid
// cell, so Qt's built-in rubber band (empty-space presses only) has nowhere
// to start. Left-button moves never reach QListWidget, because Qt's own
// drag-select state survives a swallowed release and then draws a second,
// dead rubber band on the next plain hover.
class LibraryGrid : public QListWidget {
public:
  using QListWidget::QListWidget;
  using QListWidget::setViewportMargins;

  // Set once by LibraryWindow after construction. Called with nullptr on
  // leaving a tile (hide immediately) or the viewport entirely, and with an
  // item after it's stayed hovered past the dwell below.
  std::function<void(QListWidgetItem*)> on_hover_item;

  // Ctrl+wheel resizes tiles instead of scrolling -- one call per notch,
  // positive to grow. Set once by LibraryWindow after construction.
  std::function<void(int steps)> on_ctrl_wheel;

  void SetDragSelectEnabled(bool enabled) {
    drag_select_enabled_ = enabled;
    if (!enabled) EndDrag();
  }

  // Before clear() deletes every item -- a pending dwell timer, the next
  // hover-changed check, or a drag in progress could still hold one of them.
  void ForgetItems() {
    if (hover_timer_ != nullptr) hover_timer_->stop();
    last_hover_item_ = nullptr;
    EndDrag();
  }

protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && drag_select_enabled_) {
      drag_origin_ = event->pos() + Offset();
      QListWidgetItem* pressed = itemAt(event->pos());
      press_rect_ = pressed != nullptr ? visualItemRect(pressed).translated(Offset()) : QRect();
      drag_modifiers_ = event->modifiers();
      tracking_drag_ = true;
    }
    QListWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
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
    TrackHover(itemAt(event->pos()));
    QListWidget::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    const bool was_dragging = rubber_band_ != nullptr;
    EndDrag();
    if (was_dragging) return;  // the drag already applied the selection; not a click
    QListWidget::mouseReleaseEvent(event);
  }

  void leaveEvent(QEvent* event) override {
    TrackHover(nullptr);
    QListWidget::leaveEvent(event);
  }

  void focusOutEvent(QFocusEvent* event) override {
    EndDrag();
    QListWidget::focusOutEvent(event);
  }

  void changeEvent(QEvent* event) override {
    if (event->type() == QEvent::ActivationChange && !isActiveWindow()) EndDrag();
    QListWidget::changeEvent(event);
  }

  void wheelEvent(QWheelEvent* event) override {
    if (event->modifiers() & Qt::ControlModifier) {
      // angleDelta() is in eighths of a degree; a "notch" on a real wheel is
      // 15 degrees (120), a step per notch is the usual feel for this.
      const int steps = event->angleDelta().y() / 120;
      if (steps != 0 && on_ctrl_wheel) on_ctrl_wheel(steps);
      event->accept();
      return;
    }
    QListWidget::wheelEvent(event);
    if (rubber_band_ != nullptr) UpdateDrag();
  }

private:
  // Debounced: a card popping up on every tile the cursor merely crosses
  // while scanning the grid would be worse than not having one.
  static constexpr int kHoverDwellMs = 280;
  // Within this far of the top/bottom edge (or past it), a drag scrolls.
  static constexpr int kAutoScrollEdge = 40;

  // Viewport to content coordinates, so a drag's origin stays put while
  // the grid scrolls under it.
  QPoint Offset() const { return QPoint(horizontalOffset(), verticalOffset()); }

  void TrackHover(QListWidgetItem* hovered) {
    if (hovered == last_hover_item_) return;
    last_hover_item_ = hovered;
    if (hover_timer_ == nullptr) {
      hover_timer_ = new QTimer(this);
      hover_timer_->setSingleShot(true);
      connect(hover_timer_, &QTimer::timeout, this, [this] {
        if (on_hover_item) on_hover_item(last_hover_item_);
      });
    }
    hover_timer_->stop();
    if (on_hover_item) on_hover_item(nullptr);  // hide immediately on change or leave
    if (hovered != nullptr) hover_timer_->start(kHoverDwellMs);
  }

  void UpdateDrag() {
    const QPoint current = drag_pos_ + Offset();
    if (rubber_band_ == nullptr) {
      // A press on a tile becomes a drag only once the cursor leaves that
      // tile, so a click that wobbles a few pixels stays a click.
      constexpr int kDragThreshold = 6;
      const bool started = press_rect_.isValid()
                               ? !press_rect_.contains(current)
                               : (current - drag_origin_).manhattanLength() >= kDragThreshold;
      if (!started) return;
      // A fresh drag replaces the selection unless it started with a
      // modifier held; either way, what's selected now is the floor a
      // shrinking rect won't clear again.
      if (!(drag_modifiers_ & (Qt::ControlModifier | Qt::ShiftModifier))) clearSelection();
      base_selection_.clear();
      for (QListWidgetItem* selected : selectedItems()) base_selection_.insert(selected);
      TrackHover(nullptr);  // a dwell started before the drag would pop up mid-drag
      rubber_band_ = new QRubberBand(QRubberBand::Rectangle, viewport());
      rubber_band_->show();
      if (autoscroll_timer_ == nullptr) {
        autoscroll_timer_ = new QTimer(this);
        autoscroll_timer_->setInterval(16);
        connect(autoscroll_timer_, &QTimer::timeout, this, &LibraryGrid::AutoScrollStep);
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
  void AutoScrollStep() {
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

  void EndDrag() {
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

  bool drag_select_enabled_ = true;
  bool tracking_drag_ = false;
  QPoint drag_origin_;  // content coordinates
  QRect press_rect_;    // content coordinates; invalid for a press on empty space
  QPoint drag_pos_;     // viewport coordinates, last seen
  Qt::KeyboardModifiers drag_modifiers_;  // at the press
  QRubberBand* rubber_band_ = nullptr;
  QTimer* autoscroll_timer_ = nullptr;
  QSet<QListWidgetItem*> base_selection_;
  QListWidgetItem* last_hover_item_ = nullptr;
  QTimer* hover_timer_ = nullptr;
};

namespace {

// The sidebar's filter picker, inside the filter+sort popover. Status keys
// match mirad's `status` values; "all", "running" and "never" are
// frontend-only groupings.
struct FilterEntry {
  const char* label;
  const char* key;
  mira_gui::icons::Glyph icon;
};

const FilterEntry kFilters[] = {
    {"All games", "all", mira_gui::icons::Glyph::Filter},
    {"Playing now", "running", mira_gui::icons::Glyph::Play},
    {"Ready", "ready", mira_gui::icons::Glyph::CheckCircle},
    {"Needs install", "needs_install", mira_gui::icons::Glyph::Download},
    {"Setting up", "setting_up", mira_gui::icons::Glyph::Clock},
    {"Broken", "broken", mira_gui::icons::Glyph::Warning},
    {"Missing", "missing", mira_gui::icons::Glyph::CircleX},
    {"Never played", "never", mira_gui::icons::Glyph::Moon},
    // Every entry above excludes a hidden-tagged game; this is the only one
    // that shows them, and only them.
    {"Hidden", "hidden", mira_gui::icons::Glyph::EyeSlash},
};

bool HasTag(const mira_gui::GameSummary& game, const std::string& tag) {
  return std::find(game.tags.begin(), game.tags.end(), tag) != game.tags.end();
}

// Icon + label (label also stashed in Qt::UserRole + 1, for the pill) + a
// live count (see UpdateFilterCounts). Transparent background: the list's
// own selection highlight marks the active row.
QWidget* MakeFilterRow(mira_gui::icons::Glyph glyph, const QString& label, QWidget* parent) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(6, 3, 6, 3);
  layout->setSpacing(8);
  auto* icon = new QLabel(row);
  icon->setObjectName("icon");
  icon->setPixmap(mira_gui::icons::For(glyph, mira_gui::theme::Current().text_muted).pixmap(14, 14));
  layout->addWidget(icon);
  auto* text = new QLabel(label, row);
  layout->addWidget(text, /*stretch=*/1);
  auto* count = new QLabel(row);
  count->setObjectName("count");
  count->setProperty("role", "muted");
  layout->addWidget(count);
  return row;
}

constexpr int kResizeMargin = 5;

Qt::Edges EdgesAt(const QSize& size, const QPoint& pos) {
  Qt::Edges edges;
  if (pos.x() <= kResizeMargin) edges |= Qt::LeftEdge;
  if (pos.x() >= size.width() - kResizeMargin) edges |= Qt::RightEdge;
  if (pos.y() <= kResizeMargin) edges |= Qt::TopEdge;
  if (pos.y() >= size.height() - kResizeMargin) edges |= Qt::BottomEdge;
  return edges;
}

// The frameless window's own background: a thin margin around the real
// content, the only thing left to grab for an edge resize with no OS
// titlebar. QWindow::startSystemResize hands the drag to the compositor,
// which is what makes this work under Wayland.
class RootWidget : public QWidget {
public:
  explicit RootWidget(QMainWindow* window) : window_(window) { setMouseTracking(true); }

protected:
  void mousePressEvent(QMouseEvent* event) override {
    const Qt::Edges edges = EdgesAt(size(), event->pos());
    if (event->button() == Qt::LeftButton && !window_->isMaximized() &&
        window_->windowHandle() != nullptr && edges != Qt::Edges()) {
      window_->windowHandle()->startSystemResize(edges);
      event->accept();
      return;
    }
    QWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (window_->isMaximized()) {
      unsetCursor();
      return;
    }
    const Qt::Edges edges = EdgesAt(size(), event->pos());
    if ((edges & Qt::LeftEdge) && (edges & Qt::TopEdge)) {
      setCursor(Qt::SizeFDiagCursor);
    } else if ((edges & Qt::RightEdge) && (edges & Qt::BottomEdge)) {
      setCursor(Qt::SizeFDiagCursor);
    } else if ((edges & Qt::RightEdge) && (edges & Qt::TopEdge)) {
      setCursor(Qt::SizeBDiagCursor);
    } else if ((edges & Qt::LeftEdge) && (edges & Qt::BottomEdge)) {
      setCursor(Qt::SizeBDiagCursor);
    } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
      setCursor(Qt::SizeHorCursor);
    } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
      setCursor(Qt::SizeVerCursor);
    } else {
      unsetCursor();
    }
  }

private:
  QMainWindow* window_;
};

// Sidebar's filter+sort pill. Plain QWidget, not QPushButton: needs two
// icon+label pairs and a chevron, not one icon+text. Plain callback (like
// LibraryGrid), not a signal — too small to need one.
class FilterSortButton : public QWidget {
public:
  explicit FilterSortButton(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
  }
  std::function<void()> on_clicked;

protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && on_clicked) on_clicked();
  }
};

// The game-edit card's dimmed backdrop. A click that lands here (never on
// the card itself, which is a child widget and consumes its own clicks
// first) closes the card, same as clicking outside any other modal.
class ModalOverlay : public QWidget {
public:
  explicit ModalOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true);
  }
  std::function<void()> on_backdrop_clicked;

protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && on_backdrop_clicked) on_backdrop_clicked();
  }
};

}  // namespace

LibraryWindow::LibraryWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("Mira");
  resize(1180, 720);
  // Custom top bar takes over move/resize/minimize/maximize/close — no OS
  // decoration left.
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);

  // Before the panel and the grid, because both ask it for covers.
  artwork_ = new mira_gui::ArtworkStore(this);
  connect(artwork_, &mira_gui::ArtworkStore::CoverChanged, this, &LibraryWindow::UpdateTileCover);

  // The stylesheet re-polishes every widget by itself; what it cannot reach
  // is what we paint — the tiles, and the placeholder covers drawn in the
  // theme's own colors.
  connect(mira_gui::theme::Notifier::Instance(), &mira_gui::theme::Notifier::Changed, this, [this] {
    artwork_->InvalidateAllRenderings();
    ApplyLayoutTokens();
    ApplyFilter();
    ApplyTopBarIcons();
  });

  splitter_ = new QSplitter(Qt::Horizontal, this);
  splitter_->addWidget(BuildSidebar());
  // A source page takes the grid's place here, leaving the sidebar up.
  main_stack_ = new QStackedWidget(this);
  grid_page_ = BuildGrid();
  main_stack_->addWidget(grid_page_);
  splitter_->addWidget(main_stack_);
  splitter_->setStretchFactor(0, 0);
  splitter_->setStretchFactor(1, 1);
  splitter_->setSizes({232, 850});
  splitter_->setChildrenCollapsible(false);

  // Settings is built lazily by OpenSettings() and covers this slot; the
  // classic table is built once here since it has no per-open state to go
  // stale. A game's edit card is a separate overlay, not a page here.
  content_stack_ = new QStackedWidget(this);
  content_stack_->addWidget(splitter_);
  classic_page_ = BuildClassicPage();
  content_stack_->addWidget(classic_page_);

  auto* central = new RootWidget(this);
  // StackAll: the game-edit overlay is a chrome sibling, not a
  // content_stack_ page, so the grid/sidebar stay visible (dimmed)
  // underneath. current_widget only picks which one is raised.
  root_stack_ = new QStackedLayout(central);
  root_stack_->setStackingMode(QStackedLayout::StackAll);
  root_stack_->setContentsMargins(0, 0, 0, 0);

  auto* chrome = new QWidget(central);
  auto* layout = new QVBoxLayout(chrome);
  layout->setContentsMargins(kResizeMargin, kResizeMargin, kResizeMargin, kResizeMargin);
  layout->setSpacing(0);
  QWidget* top_bar = BuildTopBar();
  // Without an explicit cursor here, a resize cursor RootWidget set at its
  // edge margin would keep showing over the whole window after the drag ends.
  top_bar->setCursor(Qt::ArrowCursor);
  layout->addWidget(top_bar);
  content_stack_->setCursor(Qt::ArrowCursor);
  layout->addWidget(content_stack_, /*stretch=*/1);
  root_stack_->addWidget(chrome);

  game_edit_overlay_ = BuildGameEditOverlay();
  root_stack_->addWidget(game_edit_overlay_);
  root_stack_->setCurrentWidget(chrome);

  setCentralWidget(central);

  // After BuildShortcuts, not before: PopulateLibraryActions reads common_'s
  // actions, which BuildShortcuts is what populates.
  BuildShortcuts();
  PopulateLibraryActions();
  UpdateLibraryNavActive();

  LoadPrefs();
  RefreshSourceNavs();
  RefreshHealth(/*force_scan=*/false);

  event_stream_.Start(this,
                      [this](std::string type, std::string data) { HandleGameEvent(type, data); });
}

void LibraryWindow::PopulateLibraryActions() {
  using mira_gui::icons::Glyph;
  QVBoxLayout* actions = library_actions_layout_;

  // Same flat row style as library_nav_ above (QSS already covers it by
  // parentage). Refresh/Shortcuts/About moved to the top bar; Close
  // window/Quit dropped (the × and tray icon already cover them).
  auto row = [this, actions](Glyph glyph, const QString& text, auto slot) {
    auto* button = new QPushButton(text, actions->parentWidget());
    button->setFlat(true);
    button->setIcon(mira_gui::icons::For(glyph));
    connect(button, &QPushButton::clicked, this, slot);
    actions->addWidget(button);
    return button;
  };

  row(Glyph::Wrench, "Runners…", &LibraryWindow::OpenRunners);
  row(Glyph::Image, "Fetch missing cover art", &LibraryWindow::FetchMissingArtwork)
      ->setToolTip(
          "Re-fetch metadata for every game with no cover. mirad only fetches automatically for a "
          "newly detected game, so a game that failed once — or a non-Steam game from before a "
          "SteamGridDB key was set — stays without one until asked again.");
  row(Glyph::Grid, "Regenerate desktop entries", &LibraryWindow::SyncDesktopEntries)
      ->setToolTip(
          "Rewrites Mira's own mira-<id>.desktop entries immediately, without waiting for the "
          "next library change to pick up a desktop_entries.* setting edit.");
  row(Glyph::Trash, "Remove all desktop entries…", &LibraryWindow::RemoveAllDesktopEntries)
      ->setToolTip("Turns off desktop entries and deletes every one Mira generated.");
}

void LibraryWindow::BuildShortcuts() {
  common_ = mira_gui::shortcuts::Install(
      this, {
                {"Ctrl+F", "Focus the search box"},
                {"Esc", "Clear the search, then the selection"},
                {"Ctrl+1…9", "Pick a filter"},
                {"Ctrl+H", "Toggle the Hidden filter"},
                {"F5, Ctrl+R", "Refresh the library"},
                {"Enter", "Play the selected game — Stop while it runs"},
                {"Alt+Enter", "Game settings"},
                {"Delete", "Remove the selected game"},
                {"Ctrl++, Ctrl+-", "Tile size"},
                {"Ctrl+0", "Reset tile size"},
                {"Ctrl+,", "Settings"},
            });

  // Each of these registers with ui/KeyBindings so Settings' Shortcuts
  // category can list and edit it — Ctrl+1…9's per-filter loop below is the
  // one deliberate exception (see its own comment).
  auto window_action = [this](const QString& id, const QString& label, QKeySequence default_keys,
                              QList<QKeySequence> extra_aliases, auto slot) {
    auto* action = new QAction(this);
    const QKeySequence primary =
        mira_gui::keybindings::Register(action, id, label, default_keys, extra_aliases);
    QList<QKeySequence> keys{primary};
    keys.append(extra_aliases);
    action->setShortcuts(keys);
    connect(action, &QAction::triggered, this, slot);
    addAction(action);
  };

  // Scoped to the grid, not the window: Delete and Enter still have to mean
  // what they mean inside the search box. WidgetWithChildrenShortcut keeps a
  // keystroke aimed at a text field from reaching the library instead.
  auto grid_action = [this](const QString& id, const QString& label, QKeySequence default_keys,
                            QList<QKeySequence> extra_aliases, auto slot) {
    auto* action = new QAction(grid_);
    const QKeySequence primary =
        mira_gui::keybindings::Register(action, id, label, default_keys, extra_aliases);
    QList<QKeySequence> keys{primary};
    keys.append(extra_aliases);
    action->setShortcuts(keys);
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(action, &QAction::triggered, this, slot);
    grid_->addAction(action);
  };

  window_action("focus_search", "Focus the search box", QKeySequence(QKeySequence::Find), {}, [this] {
    search_->setFocus(Qt::ShortcutFocusReason);
    search_->selectAll();
  });

  // One key, three jobs, in the order a user expects to undo them: leave
  // settings first, then clear the search, then clear the selection.
  window_action("clear_or_deselect", "Clear the search, then the selection",
               QKeySequence(Qt::Key_Escape), {}, [this] {
    if (SettingsOpen()) {
      RequestCloseSettings();
      return;
    }
    if (GameEditOpen()) {
      RequestCloseGameEdit();
      return;
    }
    if (!search_->text().isEmpty()) {
      search_->clear();
      return;
    }
    grid_->clearSelection();
    grid_->setCurrentItem(nullptr);
  });

  window_action("zoom_in", "Bigger tiles", QKeySequence(QKeySequence::ZoomIn),
               {QKeySequence(Qt::CTRL | Qt::Key_Equal)},
               [this] { zoom_->setValue(zoom_->value() + zoom_->pageStep()); });
  window_action("zoom_out", "Smaller tiles", QKeySequence(QKeySequence::ZoomOut), {},
               [this] { zoom_->setValue(zoom_->value() - zoom_->pageStep()); });
  window_action("reset_zoom", "Reset tile size", QKeySequence(Qt::CTRL | Qt::Key_0), {},
               [this] { zoom_->setValue(kDefaultTileWidth); });

  // Ctrl+1 through Ctrl+8, in filter order. Guarded by count() rather than
  // by kFilters so adding a ninth filter cannot walk past Ctrl+9. Not
  // registered with keybindings — nine near-identical rebindable rows for
  // "pick the Nth filter" isn't worth the Settings screen space, and the
  // filter list itself isn't fixed enough to make good default labels for.
  for (int row = 0; row < filters_->count() && row < 9; ++row) {
    auto* action = new QAction(this);
    action->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + row)));
    connect(action, &QAction::triggered, this, [this, row] { filters_->setCurrentRow(row); });
    addAction(action);
  }

  // A dedicated toggle for Hidden, on top of whatever Ctrl+9 already gives
  // it. Toggles back to All on a second press so it never strands the grid.
  window_action("toggle_hidden", "Toggle the Hidden filter", QKeySequence(Qt::CTRL | Qt::Key_H), {},
               [this] {
                 const int hidden_row = FilterRow("hidden");
                 if (hidden_row < 0) return;
                 filters_->setCurrentRow(CurrentFilterKey() == "hidden" ? FilterRow("all")
                                                                        : hidden_row);
               });

  // Neither is a menu entry anymore (both are sidebar rows now) — kept here
  // so their shortcuts and Settings-screen Shortcuts-category listing
  // survive the menu trim.
  window_action("settings", "Settings", QKeySequence(Qt::CTRL | Qt::Key_Comma), {},
               [this] { OpenSettings(); });
  // F5 is the platform's own Refresh; Ctrl+R is the one every browser
  // taught, and a second binding costs nothing. Shared id with MainWindow's
  // own refresh action -- editing either in Settings updates both.
  window_action("refresh", "Refresh the library", QKeySequence(QKeySequence::Refresh),
               {QKeySequence(Qt::CTRL | Qt::Key_R)},
               [this] { RefreshHealth(/*force_scan=*/true); });

  // Qt::Key_Enter is the keypad one — a separate key from Qt::Key_Return,
  // and binding only Return would leave it dead.
  grid_action("play_stop", "Play the selected game — Stop while it runs", QKeySequence(Qt::Key_Return),
             {QKeySequence(Qt::Key_Enter)}, [this] {
               const mira_gui::GameSummary* game = FindGame(selected_id_);
               if (game == nullptr) return;
               // Same rule as the context menu's Play entry: a game that isn't
               // ready has nothing to launch.
               if (!running_ids_.contains(game->id) && game->status != "ready") return;
               ToggleRunning(std::string(game->id));
             });

  grid_action("details_settings", "Game settings", QKeySequence(Qt::ALT | Qt::Key_Return),
             {QKeySequence(Qt::ALT | Qt::Key_Enter)}, [this] {
               if (selected_id_.empty()) return;
               OpenGameDialog(std::string(selected_id_));
             });

  grid_action("delete_game", "Remove the selected game", QKeySequence(Qt::Key_Delete), {}, [this] {
    const mira_gui::GameSummary* game = FindGame(selected_id_);
    if (game == nullptr) return;
    // Copied before the call: actions::Delete opens a modal dialog, and an
    // event arriving while it is up can refresh games_ out from under this
    // pointer.
    const std::string id = game->id;
    const QString name = QString::fromStdString(game->name);
    mira_gui::actions::Delete(this, id, name, [this] { RefreshGames(); });
  });
}

void LibraryWindow::LoadPrefs() {
  mira_gui::MiradClient::GetFrontendPrefsAsync(this, [this](mira_gui::FrontendPrefsResult result) {
    if (!result.ok) return;  // non-fatal: the built-in defaults are already applied
    const mira_gui::FrontendPrefs& prefs = result.prefs;

    if (prefs.window_width && prefs.window_height) {
      resize(*prefs.window_width, *prefs.window_height);
    }
    if (prefs.tile_width) {
      // Through the slider so its range clamp and SetTileWidth's cache
      // invalidation both apply.
      zoom_->setValue(*prefs.tile_width);
    }
    if (prefs.sidebar_width) {
      const int sidebar = *prefs.sidebar_width;
      splitter_->setSizes({sidebar, qMax(400, width() - sidebar)});
    }
    if (prefs.scan_on_startup) scan_on_startup_ = *prefs.scan_on_startup;
    if (prefs.shortcut_overrides) mira_gui::keybindings::LoadOverrides(*prefs.shortcut_overrides);
    if (prefs.sort_descending) {
      sort_descending_ = *prefs.sort_descending;
      sort_direction_->setArrowType(sort_descending_ ? Qt::DownArrow : Qt::UpArrow);
    }
    if (prefs.sort_by) {
      // Unlike the old combo box, no signal does this for us -- set the key
      // and the matching button's checked state by hand, in the same order
      // both were built in (BuildFilterSortPopover), then refresh the pill.
      sort_key_ = *prefs.sort_by;
      const std::vector<mira_gui::SortOption>& options = mira_gui::SortOptions();
      for (int i = 0; i < sort_buttons_.size() && i < static_cast<int>(options.size()); ++i) {
        sort_buttons_[i]->setChecked(options[i].key == sort_key_);
      }
      UpdateFilterSortSummary();
    }
    if (prefs.library_filter) {
      const int row = FilterRow(QString::fromStdString(*prefs.library_filter));
      if (row >= 0) filters_->setCurrentRow(row);
    }
    // Before the theme, so applying one does not repaint twice with the
    // theme's own shape first.
    mira_gui::theme::Overrides overrides;
    // Negative is how frontend.toml spells "leave it to the theme": the key
    // has to stay writable to be cleared again.
    const auto shape = [](const std::optional<int>& pref) -> std::optional<int> {
      if (pref && *pref >= 0) return pref;
      return std::nullopt;
    };
    overrides.tile_spacing = shape(prefs.tile_spacing);
    overrides.grid_margin = shape(prefs.grid_margin);
    overrides.radius_tile = shape(prefs.tile_radius);
    overrides.radius_panel = shape(prefs.panel_radius);
    overrides.radius_control = shape(prefs.control_radius);
    mira_gui::theme::SetOverrides(overrides);
    if (prefs.theme) mira_gui::theme::Apply(QString::fromStdString(*prefs.theme));
    if (prefs.game_settings_in_sidebar) game_settings_in_sidebar_ = *prefs.game_settings_in_sidebar;
    grid_->SetDragSelectEnabled(prefs.drag_select.value_or(true));
  });
}

void LibraryWindow::SavePrefs() {
  mira_gui::FrontendPrefs prefs;
  prefs.window_width = width();
  prefs.window_height = height();
  prefs.tile_width = tile_width_;
  prefs.library_filter = CurrentFilterKey().toStdString();
  prefs.sort_by = sort_key_;
  prefs.sort_descending = sort_descending_;
  prefs.scan_on_startup = scan_on_startup_;
  const QList<int> sizes = splitter_->sizes();
  if (sizes.size() == 2) prefs.sidebar_width = sizes[0];
  // Blocking, not fire-and-forget: the async form's detached thread might
  // not reach the socket before the process exits on the last window's
  // close. Failure isn't reported — the cost is a remembered layout, not data.
  mira_gui::MiradClient::SaveFrontendPrefsBlocking(prefs);
}

void LibraryWindow::ApplyLayoutTokens() {
  if (grid_ == nullptr) return;
  // Viewport margins, not the container's contents margins: those would
  // inset the whole QListWidget frame, pushing its scrollbar in by the same
  // amount. This pads only the tiles' own drawing area, leaving the
  // scrollbar docked at the panel's true right edge.
  const int margin = mira_gui::theme::Current().grid_margin;
  grid_->setViewportMargins(margin, margin, margin, margin);
}

void LibraryWindow::ApplyTopBarIcons() {
  using mira_gui::icons::Glyph;
  settings_button_->setIcon(mira_gui::icons::For(Glyph::Settings));
  refresh_button_->setIcon(mira_gui::icons::For(Glyph::Refresh));
  shortcuts_button_->setIcon(mira_gui::icons::For(Glyph::Keyboard));
  about_button_->setIcon(mira_gui::icons::For(Glyph::Info));
  top_bar_divider_->setStyleSheet(
      QString("background: %1;").arg(mira_gui::theme::Current().border.name()));
  minimize_button_->setIcon(mira_gui::icons::For(Glyph::Minimize));
  maximize_button_->setIcon(
      mira_gui::icons::For(isMaximized() ? Glyph::Restore : Glyph::Maximize));
  close_button_->setIcon(mira_gui::icons::For(Glyph::Close));
  add_games_->setIcon(mira_gui::icons::For(Glyph::Plus));

  // The filter+sort pill's own static icons -- its text and the popover's
  // rows restyle separately (UpdateFilterSortSummary, restyle_filter_rows).
  if (filter_icon_ != nullptr) {
    const QColor muted = mira_gui::theme::Current().text_muted;
    filter_icon_->setPixmap(mira_gui::icons::For(Glyph::Filter, muted).pixmap(14, 14));
    sort_icon_->setPixmap(mira_gui::icons::For(Glyph::SortArrows, muted).pixmap(13, 13));
    filter_sort_chevron_->setPixmap(mira_gui::icons::For(Glyph::ChevronDown, muted).pixmap(12, 12));
  }
  UpdateFilterSortSummary();
}

void LibraryWindow::ToggleMaximize() {
  if (isMaximized()) {
    showNormal();
  } else {
    showMaximized();
  }
}

void LibraryWindow::changeEvent(QEvent* event) {
  if (event->type() == QEvent::WindowStateChange && maximize_button_ != nullptr) {
    maximize_button_->setIcon(mira_gui::icons::For(
        isMaximized() ? mira_gui::icons::Glyph::Restore : mira_gui::icons::Glyph::Maximize));
    maximize_button_->setToolTip(isMaximized() ? "Restore" : "Maximize");
  }
  QMainWindow::changeEvent(event);
}

bool LibraryWindow::eventFilter(QObject* watched, QEvent* event) {
  // Only the top bar's own empty background reaches here — a click on a
  // child control goes to that child instead.
  if (watched == top_bar_) {
    if (event->type() == QEvent::MouseButtonPress) {
      auto* mouse = static_cast<QMouseEvent*>(event);
      if (mouse->button() == Qt::LeftButton && windowHandle() != nullptr) {
        windowHandle()->startSystemMove();
        return true;
      }
    } else if (event->type() == QEvent::MouseButtonDblClick) {
      ToggleMaximize();
      return true;
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void LibraryWindow::closeEvent(QCloseEvent* event) {
  // Only for the window Attach() made the tray's — a secondary window
  // closes for real either way, since nothing would bring it back.
  if (mira_gui::tray::IsManaged(this) && !mira_gui::tray::Quitting()) {
    SavePrefs();
    event->ignore();
    hide();
    return;
  }

  const bool settings_dirty = settings_panel_ != nullptr && settings_panel_->IsDirty();
  const bool game_dirty =
      game_edit_form_ != nullptr && GameEditOpen() && game_edit_form_->IsDirty();
  if (settings_dirty || game_dirty) {
    switch (mira_gui::notify::ConfirmUnsaved(
        this, settings_dirty ? "Settings changed but not saved."
                             : "This game's edits aren't saved.")) {
      case mira_gui::notify::UnsavedAction::Cancel:
        event->ignore();
        return;
      case mira_gui::notify::UnsavedAction::SaveAndExit:
        event->ignore();
        // Neither Save() finishes synchronously — quit for real only once it
        // has, via the one-shot below, not this closeEvent call.
        if (settings_dirty) {
          connect(settings_panel_, &mira_gui::SettingsPanel::SaveFinished, this,
                  [this](bool ok, QString) {
                    if (ok) close();
                  },
                  Qt::SingleShotConnection);
          settings_panel_->Save();
        } else {
          connect(game_edit_form_, &mira_gui::GameEditForm::SaveFinished, this,
                  [this](bool ok, QString) {
                    if (ok) close();
                  },
                  Qt::SingleShotConnection);
          game_edit_form_->Save();
        }
        return;
      case mira_gui::notify::UnsavedAction::DiscardAndExit:
        break;  // fall through to the ordinary close below
    }
  }

  SavePrefs();
  QMainWindow::closeEvent(event);
}

void LibraryWindow::OpenRunners() {
  RunnerDialog dialog(this);
  dialog.exec();
}

void LibraryWindow::OpenAbout() {
  QDialog dialog(this);
  dialog.setWindowTitle("About Mira");
  auto* layout = new QVBoxLayout(&dialog);
  layout->addWidget(new mira_gui::AboutPanel(&dialog));
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.exec();
}

void LibraryWindow::OpenGameDetailPage(const std::string& id) {
  const mira_gui::GameSummary* game = FindGame(id);
  const QString name = game != nullptr ? QString::fromStdString(game->name) : QString();
  const std::string runner_ref = game != nullptr ? game->runner_ref : std::string();
  mira_gui::GameDetailPageDialog dialog(id, name, runner_ref, this);
  dialog.exec();
}

void LibraryWindow::ScanLibrary() {
  mira_gui::MiradClient::ScanLibraryAsync(this, [this](mira_gui::ScanResult result) {
    if (!result.ok) {
      mira_gui::notify::Failed(this, "Could not scan the library.",
                               QString::fromStdString(result.error));
      return;
    }
    // New games show up in the grid on their own; only "nothing happened"
    // has no visible result of its own.
    if (result.added == 0 && result.missing == 0 && result.restored == 0) {
      mira_gui::notify::Notice(this, "Scan finished — no changes.");
    }
    RefreshGames();
  });
}

void LibraryWindow::ImportSteamLibrary() {
  mira_gui::MiradClient::ScanSteamAsync(this, [this](mira_gui::SteamScanResult result) {
    if (!result.ok) {
      mira_gui::notify::Failed(this, "Could not import from Steam.",
                               QString::fromStdString(result.error));
      return;
    }
    if (result.added == 0) mira_gui::notify::Notice(this, "No new Steam games found.");
    RefreshGames();
  });
}

void LibraryWindow::ImportLutrisLibrary() {
  mira_gui::MiradClient::ImportLutrisAsync(this, [this](mira_gui::LutrisImportResult result) {
    if (!result.ok) {
      mira_gui::notify::FailedWithHint(
          this, "Could not import from Lutris.", QString::fromStdString(result.error),
          "Lutris keeps its library in an sqlite database Mira reads with the sqlite3 command. "
          "If Lutris is installed somewhere unusual, point lutris.data_dir at it in settings.");
      return;
    }
    if (result.added == 0) mira_gui::notify::Notice(this, "No new Lutris games found.");
    RefreshGames();
  });
}

void LibraryWindow::ImportDesktopEntries() {
  mira_gui::DesktopEntryImportDialog dialog(this);
  if (dialog.exec() == QDialog::Accepted) RefreshGames();
}

void LibraryWindow::AddGameManually() {
  mira_gui::AddManualGameDialog dialog(this);
  if (dialog.exec() == QDialog::Accepted) RefreshGames();
}

void LibraryWindow::SyncDesktopEntries() {
  mira_gui::MiradClient::SyncDesktopEntriesAsync(this, [this](mira_gui::DesktopEntrySyncResult result) {
    if (!result.ok) {
      mira_gui::notify::Failed(this, "Could not regenerate desktop entries.",
                               QString::fromStdString(result.error));
      return;
    }
    mira_gui::notify::Notice(this, "Desktop entries regenerated.");
  });
}

void LibraryWindow::RemoveAllDesktopEntries() {
  // desktop_entries.enabled is the only lever that actually makes Sync()
  // remove every mira-<id>.desktop entry rather than immediately rewriting
  // them (see desktop::DesktopEntries::Sync) — there's no "wipe once, stay
  // enabled" concept, so this is honest about turning the setting off too.
  if (!mira_gui::notify::Confirm(
          this, "Remove all desktop entries",
          "This turns off desktop entries and deletes every one Mira generated. "
          "Re-enable them any time in Settings → Desktop Entries.",
          "Remove all", /*destructive=*/true)) {
    return;
  }
  const mira_gui::ConfigEdit edit{"desktop_entries.enabled", "a boolean", "false"};
  mira_gui::MiradClient::PatchConfigAsync(
      this, {edit}, [this](mira_gui::PatchConfigResult patch_result) {
        if (!patch_result.ok) {
          mira_gui::notify::Failed(this, "Could not turn off desktop entries.",
                                   QString::fromStdString(patch_result.error));
          return;
        }
        mira_gui::MiradClient::SyncDesktopEntriesAsync(
            this, [this](mira_gui::DesktopEntrySyncResult sync_result) {
              if (!sync_result.ok) {
                mira_gui::notify::Failed(this, "Could not remove the desktop entries.",
                                         QString::fromStdString(sync_result.error));
                return;
              }
              mira_gui::notify::Notice(this, "Desktop entries removed.");
            });
      });
}

QWidget* LibraryWindow::BuildTopBar() {
  top_bar_ = new QWidget(this);
  top_bar_->setObjectName("top_bar");
  // Catches a press/double-click on the bar's own empty background — see
  // eventFilter. A click on any child widget never reaches here.
  top_bar_->installEventFilter(this);

  auto* layout = new QHBoxLayout(top_bar_);
  layout->setContentsMargins(8, 4, 6, 4);
  layout->setSpacing(8);

  layout->addStretch(1);

  zoom_ = new QSlider(Qt::Horizontal, top_bar_);
  zoom_->setRange(120, 260);
  zoom_->setValue(tile_width_);
  zoom_->setMaximumWidth(120);
  zoom_->setToolTip("Tile size");
  connect(zoom_, &QSlider::valueChanged, this, &LibraryWindow::SetTileWidth);
  layout->addWidget(zoom_);

  // Moved from the sidebar's old hamburger menu -- generic actions that fit
  // the top bar (window chrome) better than a library-focused sidebar.
  refresh_button_ = new QToolButton(top_bar_);
  refresh_button_->setAutoRaise(true);
  refresh_button_->setToolTip("Refresh library");
  connect(refresh_button_, &QToolButton::clicked, this, [this] { RefreshHealth(/*force_scan=*/true); });
  layout->addWidget(refresh_button_);

  shortcuts_button_ = new QToolButton(top_bar_);
  shortcuts_button_->setAutoRaise(true);
  shortcuts_button_->setToolTip("Keyboard shortcuts");
  connect(shortcuts_button_, &QToolButton::clicked, this, [this] { common_.reference->trigger(); });
  layout->addWidget(shortcuts_button_);

  about_button_ = new QToolButton(top_bar_);
  about_button_->setAutoRaise(true);
  about_button_->setToolTip("About Mira");
  connect(about_button_, &QToolButton::clicked, this, &LibraryWindow::OpenAbout);
  layout->addWidget(about_button_);

  top_bar_divider_ = new QWidget(top_bar_);
  top_bar_divider_->setFixedSize(1, 20);
  layout->addWidget(top_bar_divider_);

  minimize_button_ = new QToolButton(top_bar_);
  minimize_button_->setAutoRaise(true);
  minimize_button_->setToolTip("Minimize");
  connect(minimize_button_, &QToolButton::clicked, this, &QWidget::showMinimized);
  layout->addWidget(minimize_button_);

  maximize_button_ = new QToolButton(top_bar_);
  maximize_button_->setAutoRaise(true);
  maximize_button_->setToolTip("Maximize");
  connect(maximize_button_, &QToolButton::clicked, this, &LibraryWindow::ToggleMaximize);
  layout->addWidget(maximize_button_);

  close_button_ = new QToolButton(top_bar_);
  close_button_->setAutoRaise(true);
  close_button_->setObjectName("close_button");
  close_button_->setToolTip("Close");
  connect(close_button_, &QToolButton::clicked, this, &QWidget::close);
  layout->addWidget(close_button_);

  ApplyTopBarIcons();
  return top_bar_;
}

QWidget* LibraryWindow::BuildFilterSortPopover() {
  using mira_gui::icons::Glyph;

  // Qt::Popup: grabs the mouse and closes itself on an outside click or
  // Escape, so the pill's on_clicked only ever needs to open it.
  auto* popover = new QWidget(this, Qt::Popup);
  popover->setObjectName("filter_sort_popover");
  auto* layout = new QVBoxLayout(popover);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(2);

  auto* filter_heading = new QLabel("FILTER", popover);
  filter_heading->setProperty("role", "muted");
  filter_heading->setStyleSheet("font-weight: 600; letter-spacing: 0.04em;");
  layout->addWidget(filter_heading);

  filters_ = new QListWidget(popover);
  filters_->setObjectName("filter_list");
  filters_->setFrameShape(QFrame::NoFrame);
  filters_->setSelectionMode(QAbstractItemView::SingleSelection);
  filters_->setFocusPolicy(Qt::NoFocus);
  filters_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  for (const FilterEntry& entry : kFilters) {
    auto* item = new QListWidgetItem(filters_);
    item->setData(Qt::UserRole, QString(entry.key));
    // Stashed alongside the key so the pill can show it without digging
    // back into the row widget's own child labels.
    item->setData(Qt::UserRole + 1, QString(entry.label));
    auto* row = MakeFilterRow(entry.icon, entry.label, filters_);
    item->setSizeHint(row->sizeHint());
    filters_->setItemWidget(item, row);
  }
  // setItemWidget bypasses QSS's ::item:selected -- restyled by hand
  // instead (icon included), on_accent when active, else muted.
  auto restyle_filter_rows = [this] {
    for (int row = 0; row < filters_->count(); ++row) {
      QWidget* row_widget = filters_->itemWidget(filters_->item(row));
      const bool current = row == filters_->currentRow();
      const QColor color = current ? mira_gui::theme::Current().on_accent
                                   : mira_gui::theme::Current().text_muted;
      for (QLabel* label : row_widget->findChildren<QLabel*>()) {
        if (label->objectName() == "icon") {
          label->setPixmap(mira_gui::icons::For(kFilters[row].icon, color).pixmap(14, 14));
        } else {
          // Count included: on_accent for contrast against the accent
          // background, not left at its ordinary muted gray.
          label->setStyleSheet(current ? QString("color: %1;").arg(color.name()) : QString());
        }
      }
    }
  };
  filters_->setCurrentRow(0);
  restyle_filter_rows();
  connect(filters_, &QListWidget::currentRowChanged, this, [this, restyle_filter_rows] {
    restyle_filter_rows();
    UpdateFilterSortSummary();
    ApplyFilter();
  });
  // QListWidget's own sizeHint doesn't grow with its item count -- fit
  // exactly the rows it has, once, rather than an arbitrary scrollable box.
  int filters_height = 2 * filters_->frameWidth();
  for (int row = 0; row < filters_->count(); ++row) filters_height += filters_->sizeHintForRow(row);
  filters_->setFixedHeight(filters_height);
  layout->addWidget(filters_);

  auto* divider = new QWidget(popover);
  divider->setFixedHeight(1);
  divider->setStyleSheet(QString("background: %1;").arg(mira_gui::theme::Current().border.name()));
  layout->addWidget(divider);

  auto* sort_heading_row = new QHBoxLayout();
  auto* sort_heading = new QLabel("SORT", popover);
  sort_heading->setProperty("role", "muted");
  sort_heading->setStyleSheet("font-weight: 600; letter-spacing: 0.04em;");
  sort_heading_row->addWidget(sort_heading, /*stretch=*/1);

  sort_direction_ = new QToolButton(popover);
  sort_direction_->setAutoRaise(true);
  sort_direction_->setIcon(mira_gui::icons::For(mira_gui::icons::Glyph::SortArrows));
  sort_direction_->setToolTip(sort_descending_ ? "Descending — click for ascending"
                                               : "Ascending — click for descending");
  connect(sort_direction_, &QToolButton::clicked, this, [this] {
    sort_descending_ = !sort_descending_;
    sort_direction_->setToolTip(sort_descending_ ? "Descending — click for ascending"
                                                 : "Ascending — click for descending");
    UpdateFilterSortSummary();
    ApplyFilter();
  });
  sort_heading_row->addWidget(sort_direction_);
  layout->addLayout(sort_heading_row);

  // Full-width rows, same shape as the filter list above (and the sidebar's
  // own nav rows) -- a segmented row cramped "Last played"/"Playtime" down
  // to unreadable widths at the sidebar's ~230px.
  auto* sort_group = new QButtonGroup(popover);
  sort_buttons_.clear();
  for (const mira_gui::SortOption& option : mira_gui::SortOptions()) {
    auto* button = new QPushButton(option.label, popover);
    button->setFlat(true);
    button->setCheckable(true);
    button->setChecked(option.key == sort_key_);
    const QString key = QString(option.key);
    connect(button, &QPushButton::clicked, this, [this, key] {
      sort_key_ = key.toStdString();
      UpdateFilterSortSummary();
      ApplyFilter();
    });
    sort_group->addButton(button);
    sort_buttons_.append(button);
    layout->addWidget(button);
  }

  return popover;
}

void LibraryWindow::UpdateFilterSortSummary() {
  if (filter_summary_label_ == nullptr) return;  // popover not built yet

  const QListWidgetItem* current = filters_->currentItem();
  filter_summary_label_->setText(current != nullptr ? current->data(Qt::UserRole + 1).toString()
                                                     : QString("All games"));

  QString sort_label = "Name";
  for (const mira_gui::SortOption& option : mira_gui::SortOptions()) {
    if (option.key == sort_key_) {
      sort_label = option.label;
      break;
    }
  }
  sort_summary_label_->setText(
      QString("%1 %2").arg(sort_label, sort_descending_ ? QString::fromUtf8("\xe2\x86\x93")
                                                        : QString::fromUtf8("\xe2\x86\x91")));
}

QWidget* LibraryWindow::BuildSidebar() {
  auto* sidebar = new QWidget(this);
  sidebar->setObjectName("left_sidebar");
  auto* layout = new QVBoxLayout(sidebar);
  layout->setContentsMargins(10, 14, 10, 10);
  layout->setSpacing(2);

  // Always visible (not just a "back" affordance): checked/highlighted
  // exactly when the grid is the current content — see UpdateLibraryNavActive.
  library_nav_ = new QPushButton("Library", sidebar);
  library_nav_->setObjectName("library_nav");
  library_nav_->setFlat(true);
  library_nav_->setCheckable(true);
  library_nav_->setChecked(true);
  connect(library_nav_, &QPushButton::clicked, this, [this] {
    if (SettingsOpen()) {
      RequestCloseSettings();
    } else if (GameEditOpen()) {
      RequestCloseGameEdit();
    } else if (content_stack_->currentWidget() == classic_page_) {
      CloseClassicView();
    } else if (source_page_ != nullptr) {
      CloseSource();
    }
  });
  layout->addWidget(library_nav_);

  classic_view_nav_ = new QPushButton("Classic table view", sidebar);
  classic_view_nav_->setObjectName("classic_view_nav");
  classic_view_nav_->setFlat(true);
  classic_view_nav_->setCheckable(true);
  connect(classic_view_nav_, &QPushButton::clicked, this, &LibraryWindow::OpenClassicView);
  layout->addWidget(classic_view_nav_);

  layout->addSpacing(10);

  add_games_ = new QToolButton(sidebar);
  add_games_->setObjectName("add_games");
  add_games_->setText("Add/Import Games");
  add_games_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  add_games_->setPopupMode(QToolButton::InstantPopup);
  // QToolButton's own sizeHint is Preferred but stays content-sized in
  // practice next to a QPushButton row with the same nominal policy —
  // Expanding makes it actually stretch to the sidebar's full width.
  add_games_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* add_games_menu = new QMenu(add_games_);
  add_games_menu->addAction("Scan library folders", this, &LibraryWindow::ScanLibrary);
  add_games_menu->addAction("Import Steam library", this, &LibraryWindow::ImportSteamLibrary);
  add_games_menu
      ->addAction("Import Lutris games", this, &LibraryWindow::ImportLutrisLibrary)
      ->setToolTip(
          "Read Lutris's own database and add its Wine games here. Nothing is moved or renamed, "
          "in either launcher's files — a game stays playable in Lutris too.");
  add_games_menu
      ->addAction("Import desktop entries…", this, &LibraryWindow::ImportDesktopEntries)
      ->setToolTip(
          "Pick from already-installed application-menu entries — including Flatpak apps, via "
          "their own X-Flatpak key.");
  add_games_menu->addSeparator();
  add_games_menu->addAction("Add game manually…", this, &LibraryWindow::AddGameManually);
  add_games_->setMenu(add_games_menu);
  layout->addWidget(add_games_);

  layout->addSpacing(10);

  search_ = new QLineEdit(sidebar);
  search_->setObjectName("library_search");
  search_->setPlaceholderText("Search…");
  search_->setClearButtonEnabled(true);
  connect(search_, &QLineEdit::textChanged, this, [this] { ApplyFilter(); });
  layout->addWidget(search_);

  layout->addSpacing(10);

  // Pill summarizing filter+sort, opening a self-dismissing Qt::Popup with
  // the actual rows/buttons.
  filter_sort_popover_ = BuildFilterSortPopover();

  auto* pill = new FilterSortButton(sidebar);
  pill->setObjectName("filter_sort_button");
  filter_sort_button_ = pill;
  auto* pill_layout = new QHBoxLayout(pill);
  pill_layout->setContentsMargins(8, 6, 8, 6);
  pill_layout->setSpacing(6);

  filter_icon_ = new QLabel(pill);
  pill_layout->addWidget(filter_icon_);
  filter_summary_label_ = new QLabel(pill);
  pill_layout->addWidget(filter_summary_label_, /*stretch=*/1);

  auto* pill_divider = new QWidget(pill);
  pill_divider->setFixedSize(1, 14);
  pill_divider->setStyleSheet(QString("background: %1;").arg(mira_gui::theme::Current().border.name()));
  pill_layout->addWidget(pill_divider);

  sort_icon_ = new QLabel(pill);
  pill_layout->addWidget(sort_icon_);
  sort_summary_label_ = new QLabel(pill);
  sort_summary_label_->setProperty("role", "muted");
  pill_layout->addWidget(sort_summary_label_);

  filter_sort_chevron_ = new QLabel(pill);
  pill_layout->addWidget(filter_sort_chevron_);

  pill->on_clicked = [this] {
    filter_sort_popover_->setFixedWidth(filter_sort_button_->width());
    const QPoint below_left =
        filter_sort_button_->mapToGlobal(QPoint(0, filter_sort_button_->height() + 4));
    filter_sort_popover_->move(below_left);
    filter_sort_popover_->show();
  };
  layout->addWidget(pill);

  // The LIBRARY and SOURCES rows scroll, so they never set the window's
  // minimum height.
  layout->addSpacing(14);
  auto* nav_scroll = new QScrollArea(sidebar);
  nav_scroll->setWidgetResizable(true);
  nav_scroll->setFrameShape(QFrame::NoFrame);
  nav_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  nav_scroll->viewport()->setAutoFillBackground(false);
  auto* nav_content = new QWidget();
  nav_content->setAutoFillBackground(false);
  auto* nav_layout = new QVBoxLayout(nav_content);
  nav_layout->setContentsMargins(0, 0, 0, 0);
  nav_layout->setSpacing(2);

  const auto heading = [nav_content, nav_layout](const QString& text) {
    auto* label = new QLabel(text, nav_content);
    label->setProperty("role", "muted");
    label->setStyleSheet("font-weight: 600; letter-spacing: 0.04em;");
    nav_layout->addWidget(label);
  };
  heading("LIBRARY");
  // Filled in by PopulateLibraryActions(), after BuildShortcuts() populates
  // common_.
  library_actions_layout_ = new QVBoxLayout();
  library_actions_layout_->setSpacing(2);
  nav_layout->addLayout(library_actions_layout_);

  nav_layout->addSpacing(14);
  heading("SOURCES");
  for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
    auto* nav = new QPushButton(source.name, nav_content);
    nav->setFlat(true);
    nav->setCheckable(true);
    connect(nav, &QPushButton::clicked, this, [this, source] { OpenSource(source); });
    nav_layout->addWidget(nav);
    source_navs_.append(nav);
  }
  nav_layout->addStretch(1);
  nav_scroll->setWidget(nav_content);
  layout->addWidget(nav_scroll, /*stretch=*/1);

  settings_button_ = new QPushButton("Settings", sidebar);
  settings_button_->setObjectName("sidebar_settings");
  settings_button_->setFlat(true);
  connect(settings_button_, &QPushButton::clicked, this, [this] { OpenSettings(); });
  layout->addWidget(settings_button_);

  // Replaces the old bottom bar entirely.
  footer_ = new QLabel(sidebar);
  footer_->setProperty("role", "muted");
  footer_->setContentsMargins(6, 8, 6, 2);
  footer_->setWordWrap(true);
  layout->addWidget(footer_);

  return sidebar;
}

QWidget* LibraryWindow::BuildGrid() {
  auto* container = new QWidget(this);
  grid_layout_ = new QVBoxLayout(container);
  QVBoxLayout* layout = grid_layout_;
  layout->setSpacing(6);

  grid_ = new LibraryGrid(container);
  grid_->setObjectName("library_grid");
  delegate_ = new mira_gui::GameTileDelegate(grid_, TileSize());
  grid_->setItemDelegate(delegate_);
  grid_->setViewMode(QListView::IconMode);
  grid_->setResizeMode(QListView::Adjust);
  grid_->setMovement(QListView::Static);
  grid_->setUniformItemSizes(true);
  grid_->setSpacing(0);
  grid_->setGridSize(TileSize());
  // Extended, not Single: ctrl/shift-click and a drag over empty space
  // (rubber-band, built into QAbstractItemView for this mode) both select
  // more than one tile, for the batch actions in ShowContextMenu.
  grid_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  grid_->setMouseTracking(true);
  grid_->setFrameShape(QFrame::NoFrame);
  grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // Default QListView scrolling moves one item per wheel tick, a visible
  // jump for a 250px tile. Per-pixel smooths it; not also grabbing a
  // QScroller drag gesture, which would fight single-click-select.
  grid_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  grid_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(grid_, &QListWidget::itemSelectionChanged, this, &LibraryWindow::SelectionChanged);
  connect(grid_, &QListWidget::customContextMenuRequested, this, &LibraryWindow::ShowContextMenu);
  connect(grid_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
    const std::string id = item->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString();
    const std::string status =
        item->data(mira_gui::GameTileDelegate::StatusRole).toString().toStdString();
    // Same rule as the context menu's Play entry and the Enter shortcut: a
    // game that isn't ready has nothing to launch, and /launch would just
    // 409. Stop needs no such guard — running_ids_ already reflects reality.
    if (!running_ids_.contains(id) && status != "ready") return;
    ToggleRunning(id);
  });
  grid_->on_hover_item = [this](QListWidgetItem* item) { ShowHoverCard(item); };
  grid_->on_ctrl_wheel = [this](int steps) {
    zoom_->setValue(zoom_->value() + steps * zoom_->pageStep());
  };
  layout->addWidget(grid_, /*stretch=*/1);
  ApplyLayoutTokens();  // needs grid_ to already exist

  empty_hint_ = new QLabel(container);
  empty_hint_->setAlignment(Qt::AlignCenter);
  empty_hint_->setProperty("role", "muted");
  empty_hint_->setVisible(false);
  layout->addWidget(empty_hint_);

  return container;
}

QSize LibraryWindow::TileSize() const {
  // 2:3 portrait cover ratio, plus room for the title band over the bottom.
  return QSize(tile_width_, tile_width_ * 3 / 2);
}

void LibraryWindow::SetTileWidth(int width) {
  if (width == tile_width_) return;
  tile_width_ = width;
  delegate_->SetTileSize(TileSize());
  grid_->setGridSize(TileSize());
  ApplyFilter();
}

QPixmap LibraryWindow::CoverFor(const mira_gui::GameSummary& game) {
  return artwork_->Cover(game, TileSize(), devicePixelRatioF());
}

void LibraryWindow::UpdateTileCover(const QString& id) {
  if (source_page_ != nullptr) source_page_->UpdateCover(id);
  // One item, not a rebuild: artwork arrives one game at a time, and
  // ApplyFilter would drop the selection and scroll position on each.
  for (int row = 0; row < grid_->count(); ++row) {
    QListWidgetItem* item = grid_->item(row);
    if (item->data(mira_gui::GameTileDelegate::IdRole).toString() != id) continue;
    const mira_gui::GameSummary* game = FindGame(id.toStdString());
    if (game == nullptr) return;
    item->setData(Qt::DecorationRole, CoverFor(*game));
    // The edit page draws the same game at a different size, so it needs the
    // same nudge — it has no way to notice the store changed under it. A
    // no-op if it isn't currently showing this game (or isn't open at all).
    if (game_edit_form_ != nullptr) game_edit_form_->RefreshCover();
    return;
  }
}

void LibraryWindow::ShowSteamGridDbNotice(bool asked_for) {
  // Only when the user actually asked for art — a background fetch after a
  // scan hitting this would otherwise nag on every launch. Once per session,
  // however many games report it.
  if (!asked_for || steamgriddb_notice_shown_) return;
  steamgriddb_notice_shown_ = true;

  mira_gui::notify::FailedWithAction(
      this, "No SteamGridDB API key set.",
      "Non-Steam games need a free SteamGridDB API key before Mira can find cover art for "
      "them — there is no other free source for one. Steam games are unaffected.",
      QString(), "Open the Metadata settings…", [this] { OpenSettings("steamgriddb.api_key"); });
}

void LibraryWindow::FetchMissingArtwork() {
  // One request for the whole library; mirad decides what's missing.
  mira_gui::MiradClient::RefreshMissingArtworkAsync(
      this, [this](mira_gui::RefreshMissingArtworkResult result) {
        if (!result.ok) {
          mira_gui::notify::Failed(this, "Could not fetch missing cover art.",
                                   QString::fromStdString(result.error));
          return;
        }
        if (result.count == 0) {
          mira_gui::notify::Notice(this, "Every game already has cover art.");
          return;
        }
        // No notice: covers appear as they arrive. Asked-for, though, so a
        // missing SteamGridDB key is worth saying (ShowSteamGridDbNotice).
        artwork_fetch_requested_ = true;
      });
}

void LibraryWindow::RefreshMetadata(const std::string& id, bool announce) {
  mira_gui::MiradClient::RefreshMetadataAsync(
      this, id, announce, [this, id, announce](mira_gui::MetadataRefreshResult result) {
        if (!result.ok) {
          if (announce) {
            mira_gui::notify::Failed(this, "Could not refresh metadata.",
                                     QString::fromStdString(result.error));
          }
          return;
        }
        // 202: fetch runs on the daemon, reports back as an event. Remembered
        // so ShowSteamGridDbNotice knows this game was asked about.
        awaiting_metadata_.insert(id);
      });
}

void LibraryWindow::OpenArtworkPicker(const std::string& id, const std::string& slot) {
  // Its own window, not modal: browsing candidates works better alongside
  // the grid than blocking it, and each one is a throwaway (WA_DeleteOnClose)
  // rather than something worth tracking and reusing.
  auto* picker = new mira_gui::ArtworkPickerDialog(id, slot, this);
  picker->setAttribute(Qt::WA_DeleteOnClose);
  picker->show();
}

void LibraryWindow::RefreshHealth(bool force_scan) {
  mira_gui::MiradClient::CheckHealthAsync(this, [this, force_scan](mira_gui::HealthStatus status) {
    if (status.reachable) {
      RescanAndRefreshGames(force_scan);
    } else {
      games_.clear();
      ApplyFilter();
      mira_gui::notify::FailedWithHint(
          this, "Could not reach mirad.", QString::fromStdString(status.detail),
          "It was reachable when this window opened, so something stopped it. If you started "
          "it yourself, run \"mirad\" again in a terminal. If systemd manages it: "
          "systemctl --user restart mirad — and journalctl --user -u mirad to see why it "
          "stopped.");
    }
  });
}

void LibraryWindow::RescanAndRefreshGames(bool force_scan) {
  if (!force_scan && !scan_on_startup_) {
    // mirad's own watcher keeps the library current while it runs, so
    // skipping the startup scan costs nothing except a library that
    // changed while mirad was stopped.
    RefreshGames();
    return;
  }
  if (!loaded_) {
    // First load: a scan only reports changes, not what already existed.
    mira_gui::MiradClient::ScanLibraryAsync(this, [this](mira_gui::ScanResult) { RefreshGames(); });
    return;
  }
  // Kept in sync since by game.added/.updated/.removed events.
  mira_gui::MiradClient::ScanLibraryAsync(this, [](mira_gui::ScanResult) {});
}

void LibraryWindow::RefreshGames() {
  // Two fetches: mirad leaves hidden-tagged games out of the bare list;
  // ?tag=hidden is the only call that returns them. Both land in games_ up
  // front so Ctrl+H is a client-side filter switch, not a round trip.
  mira_gui::MiradClient::ListGamesAsync(this, [this](mira_gui::GamesResult visible) {
    if (!visible.ok) {
      mira_gui::notify::Failed(this, "Could not list games.",
                               QString::fromStdString(visible.error));
      games_.clear();
      ApplyFilter();
      return;
    }
    mira_gui::MiradClient::ListGamesAsync(
        this,
        [this, visible = std::move(visible)](mira_gui::GamesResult hidden) mutable {
          games_ = std::move(visible.games);
          // A failed second fetch just means the Hidden filter shows
          // nothing this round — not worth failing the whole refresh over.
          if (hidden.ok) {
            for (mira_gui::GameSummary& game : hidden.games) games_.push_back(std::move(game));
          }
          loaded_ = true;
          ApplyFilter();
        },
        /*status_filter=*/std::string(), /*tag_filter=*/"hidden");
  });
}

QString LibraryWindow::CurrentFilterKey() const {
  QListWidgetItem* item = filters_->currentItem();
  return item != nullptr ? item->data(Qt::UserRole).toString() : QString("all");
}

int LibraryWindow::FilterRow(const QString& key) const {
  for (int row = 0; row < filters_->count(); ++row) {
    if (filters_->item(row)->data(Qt::UserRole).toString() == key) return row;
  }
  return -1;
}

bool LibraryWindow::MatchesFilter(const mira_gui::GameSummary& game) const {
  const QString search = search_->text().trimmed();
  if (!search.isEmpty() &&
      !QString::fromStdString(game.name).contains(search, Qt::CaseInsensitive)) {
    return false;
  }
  return MatchesFilterKey(game, CurrentFilterKey());
}

bool LibraryWindow::MatchesFilterKey(const mira_gui::GameSummary& game, const QString& key) const {
  if (key == "hidden") return HasTag(game, "hidden");
  // Every other filter excludes a hidden game — "not displayed by default"
  // means not in "All games" either, not just off the initial screen.
  if (HasTag(game, "hidden")) return false;
  if (key == "all") return true;
  if (key == "running") return running_ids_.contains(game.id);
  if (key == "never") return !game.last_played_at.has_value();
  return key.toStdString() == game.status;
}

void LibraryWindow::UpdateFilterCounts() {
  for (int row = 0; row < filters_->count(); ++row) {
    QListWidgetItem* item = filters_->item(row);
    const QString key = item->data(Qt::UserRole).toString();
    int count = 0;
    for (const mira_gui::GameSummary& game : games_) {
      if (MatchesFilterKey(game, key)) ++count;
    }
    auto* count_label = qobject_cast<QLabel*>(filters_->itemWidget(item)->findChild<QLabel*>("count"));
    if (count_label != nullptr) count_label->setText(QString::number(count));
  }
}

void LibraryWindow::ApplyFilter() {
  const std::string previously_selected = selected_id_;
  UpdateFilterCounts();

  // Sorted here, not at fetch time, so a sort change costs a tile rebuild,
  // not a round trip.
  mira_gui::SortGames(games_, sort_key_, sort_descending_);

  // grid_->clear() deletes every item; a stale last_hover_item_/pending
  // dwell timer pointing at one of them would be a use-after-free the next
  // time it fires.
  grid_->ForgetItems();
  ShowHoverCard(nullptr);

  grid_->blockSignals(true);
  grid_->clear();

  int shown = 0;
  QListWidgetItem* to_select = nullptr;
  for (const mira_gui::GameSummary& game : games_) {
    if (!MatchesFilter(game)) continue;
    ++shown;

    auto* item = new QListWidgetItem(grid_);
    item->setData(mira_gui::GameTileDelegate::IdRole, QString::fromStdString(game.id));
    item->setData(mira_gui::GameTileDelegate::NameRole, QString::fromStdString(game.name));
    item->setData(mira_gui::GameTileDelegate::StatusRole, QString::fromStdString(game.status));
    item->setData(mira_gui::GameTileDelegate::RunningRole, running_ids_.contains(game.id));
    item->setData(Qt::DecorationRole, CoverFor(game));
    // Only for an error: the plain name case is already covered by the
    // HoverCard, and Qt's own tooltip popping up alongside it just doubled
    // up on the same text in a worse-looking box.
    if (!game.last_error.empty()) {
      item->setToolTip(QString("%1\n%2").arg(QString::fromStdString(game.name),
                                             QString::fromStdString(game.last_error)));
    }
    if (game.id == previously_selected) to_select = item;
  }
  grid_->blockSignals(false);

  if (to_select != nullptr) {
    grid_->setCurrentItem(to_select);
  } else if (!previously_selected.empty()) {
    // Selected game was filtered away or removed — don't keep showing it.
    selected_id_.clear();
  }

  empty_hint_->setVisible(shown == 0);
  if (shown == 0) {
    empty_hint_->setText(games_.empty() ? "No games in the library yet."
                                        : "No games match this filter.");
  }

  // Two lines: count first (what you're looking at), connection status
  // second (background fact, muted further by the dot standing in for a
  // word). Only reached after a successful fetch, so the dot is always
  // the "connected" color -- there's no "shown, but not connected" state.
  footer_->setText(QString("%1 of %2 games shown<br><span style='color:%3'>●</span> Connected via %4")
                       .arg(shown)
                       .arg(games_.size())
                       .arg(mira_gui::theme::Current().success.name())
                       .arg(QString::fromStdString(mira_gui::MiradClient::ResolveSocketPath())));

  RefreshClassicTable();
  if (source_page_ != nullptr) source_page_->SetGames(games_, running_ids_);
}

const mira_gui::GameSummary* LibraryWindow::FindGame(const std::string& id) const {
  for (const mira_gui::GameSummary& game : games_) {
    if (game.id == id) return &game;
  }
  return nullptr;
}

void LibraryWindow::UpsertGame(const mira_gui::GameSummary& game) {
  // A rename changes the placeholder's initials, so the rendered tile is
  // stale even though the fetched artwork behind it isn't.
  artwork_->InvalidateRendering(game.id);

  for (mira_gui::GameSummary& existing : games_) {
    if (existing.id == game.id) {
      existing = game;
      ApplyFilter();
      return;
    }
  }
  games_.push_back(game);
  ApplyFilter();
}

void LibraryWindow::RemoveGame(const std::string& id) {
  for (auto it = games_.begin(); it != games_.end(); ++it) {
    if (it->id == id) {
      games_.erase(it);
      break;
    }
  }
  if (selected_id_ == id) selected_id_.clear();
  ApplyFilter();
}

void LibraryWindow::SelectionChanged() {
  // Not the grid on screen (Settings or classic table instead) — a stray
  // signal (e.g. ApplyFilter rebuilding the grid) should stay a no-op.
  if (!GridShown()) return;

  const QList<QListWidgetItem*> selected = grid_->selectedItems();
  selected_id_ = selected.size() == 1
                     ? selected.first()->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString()
                     : std::string();
}

void LibraryWindow::ShowHoverCard(QListWidgetItem* item) {
  if (item == nullptr) {
    if (hover_card_ != nullptr) hover_card_->hide();
    return;
  }
  // Nothing to preview once the grid isn't on screen, and a preview for one
  // game reads as wrong noise over an active multi-selection.
  if (!GridShown() || grid_->selectedItems().size() > 1) return;

  const std::string id = item->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString();
  const mira_gui::GameSummary* game = FindGame(id);
  if (game == nullptr) return;

  if (hover_card_ == nullptr) hover_card_ = new mira_gui::HoverCard(this);
  hover_card_->ShowGame(*game, running_ids_.contains(id));
  hover_card_->adjustSize();

  const QRect tile_rect = grid_->visualItemRect(item);
  const QPoint top_right = grid_->viewport()->mapToGlobal(tile_rect.topRight());
  QPoint pos(top_right.x() + 8, top_right.y());
  // To the right of the tile by default; the left side instead if that
  // would run off the screen (a tile in the grid's rightmost column).
  if (QScreen* screen = QGuiApplication::screenAt(top_right)) {
    if (pos.x() + hover_card_->width() > screen->availableGeometry().right()) {
      const QPoint top_left = grid_->viewport()->mapToGlobal(tile_rect.topLeft());
      pos.setX(top_left.x() - hover_card_->width() - 8);
    }
  }
  hover_card_->move(pos);
  hover_card_->show();
}

void LibraryWindow::ShowContextMenu(const QPoint& pos) {
  QListWidgetItem* item = grid_->itemAt(pos);
  if (item == nullptr) return;
  // Right-clicking outside the current selection replaces it, same as most
  // file managers; right-clicking inside a multi-selection keeps it so the
  // batch menu below applies to everything that was selected.
  if (!item->isSelected()) {
    grid_->clearSelection();
    item->setSelected(true);
  }
  grid_->setCurrentItem(item);

  if (grid_->selectedItems().size() > 1) {
    ShowBatchContextMenu(grid_->selectedItems(), pos);
    return;
  }

  const std::string id = item->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString();
  const QString name = item->data(mira_gui::GameTileDelegate::NameRole).toString();
  const std::string status =
      item->data(mira_gui::GameTileDelegate::StatusRole).toString().toStdString();
  const bool running = running_ids_.contains(id);
  const mira_gui::GameSummary* current_game = FindGame(id);

  QMenu menu(this);
  QAction* play = menu.addAction(running ? "Stop" : "Play");
  play->setEnabled(running || status == "ready");
  QAction* details = menu.addAction("Game settings…");
  QAction* folder = menu.addAction("Open install folder");
  QAction* more_details = menu.addAction("More details…");
  menu.addSeparator();
  // Both halves of the needs_install escape hatch: run the installer inside
  // this game's prefix, then say it worked. "Run in prefix" is offered for
  // every game; only needs_install can be "marked installed".
  QAction* run_in_prefix = menu.addAction("Run in prefix…");
  QAction* finish_install = menu.addAction("Mark as installed");
  finish_install->setEnabled(status == "needs_install");
  finish_install->setToolTip(status == "needs_install"
                                 ? "Flip this game to ready once its executable points at the "
                                   "installed program"
                                 : "Only applies to a game that still needs installing");
  QAction* refresh_metadata = menu.addAction("Refresh metadata && cover art");
  QAction* view_log = menu.addAction("View log…");
  QAction* winetricks = menu.addAction("Run winetricks…");
  const bool native = current_game != nullptr && current_game->platform == "native";
  winetricks->setEnabled(!native);
  winetricks->setToolTip(native ? "Native game — no Wine/Proton prefix." : QString());
  // Resolved (not on GameSummary), and the menu item's own label is the only
  // place that state shows, so it's fetched synchronously here rather than
  // asking first and acting second — a local socket round trip, once, before
  // the menu is shown.
  bool desktop_entry_enabled = true;
  {
    QEventLoop loop;
    mira_gui::MiradClient::GetGameConfigAsync(
        this, id, [&desktop_entry_enabled, &loop](mira_gui::GameConfigResult result) {
          if (result.ok) {
            for (const mira_gui::GameConfigEntry& entry : result.entries) {
              if (entry.key == "desktop_entries.enabled") {
                desktop_entry_enabled = entry.value_display == "true";
                break;
              }
            }
          }
          loop.quit();
        });
    loop.exec();
  }
  QAction* desktop_entry =
      menu.addAction(desktop_entry_enabled ? "Remove desktop entry" : "Add desktop entry");
  desktop_entry->setToolTip("Whether this game has its own entry in the application menu.");
  menu.addSeparator();
  const bool hidden = current_game != nullptr && HasTag(*current_game, "hidden");
  QAction* toggle_hidden = menu.addAction(hidden ? "Unhide" : "Hide");
  toggle_hidden->setToolTip(hidden
                                ? "Show this game in the library again"
                                : "Keep this game out of the library until you ask for it "
                                  "(Ctrl+H, or the Hidden filter)");
  menu.addSeparator();
  QAction* remove = menu.addAction("Remove from library…");

  QAction* chosen = menu.exec(grid_->viewport()->mapToGlobal(pos));
  if (chosen == play) {
    ToggleRunning(id);
  } else if (chosen == details) {
    OpenGameDialog(id);
  } else if (chosen == folder) {
    mira_gui::actions::OpenInstallFolder(
        this, current_game != nullptr ? current_game->install_path : std::string());
  } else if (chosen == more_details) {
    OpenGameDetailPage(id);
  } else if (chosen == run_in_prefix) {
    mira_gui::actions::RunInPrefix(
        this, id, current_game != nullptr ? current_game->install_path : std::string(), name);
  } else if (chosen == finish_install) {
    mira_gui::actions::FinishInstall(this, id, [this] { RefreshGames(); });
  } else if (chosen == refresh_metadata) {
    RefreshMetadata(id);
  } else if (chosen == view_log) {
    mira_gui::actions::ViewLog(this, id, name);
  } else if (chosen == winetricks) {
    mira_gui::actions::RunWinetricks(this, id, name);
  } else if (chosen == desktop_entry) {
    mira_gui::actions::ToggleDesktopEntry(this, id, desktop_entry_enabled);
  } else if (chosen == toggle_hidden) {
    ToggleHidden(id);
  } else if (chosen == remove) {
    mira_gui::actions::Delete(this, id, name, [this] { RefreshGames(); });
  }
}

void LibraryWindow::ShowBatchContextMenu(const QList<QListWidgetItem*>& items, const QPoint& pos) {
  const int count = items.size();

  QMenu menu(this);
  QAction* refresh_metadata = menu.addAction(QString("Refresh metadata && cover art (%1)").arg(count));
  QAction* hide = menu.addAction(QString("Hide (%1)").arg(count));
  hide->setToolTip(
      "Keep these games out of the library until asked for (Ctrl+H, or the Hidden filter)");
  auto* desktop_menu = menu.addMenu("Desktop entry");
  QAction* add_desktop_entry = desktop_menu->addAction("Add to application menu");
  QAction* remove_desktop_entry = desktop_menu->addAction("Remove from application menu");
  menu.addSeparator();
  QAction* remove = menu.addAction(QString("Remove from library… (%1)").arg(count));

  QAction* chosen = menu.exec(grid_->viewport()->mapToGlobal(pos));

  std::vector<std::string> ids;
  std::vector<std::pair<std::string, QString>> named;
  ids.reserve(count);
  named.reserve(count);
  for (QListWidgetItem* item : items) {
    ids.push_back(item->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString());
    named.emplace_back(ids.back(), item->data(mira_gui::GameTileDelegate::NameRole).toString());
  }

  if (chosen == refresh_metadata) {
    // No notice: covers visibly update as each fetch lands.
    for (const std::string& game_id : ids) RefreshMetadata(game_id, /*announce=*/false);
  } else if (chosen == hide) {
    BatchHide(ids);
  } else if (chosen == add_desktop_entry) {
    mira_gui::actions::BatchSetDesktopEntry(this, ids, /*enabled=*/true);
  } else if (chosen == remove_desktop_entry) {
    mira_gui::actions::BatchSetDesktopEntry(this, ids, /*enabled=*/false);
  } else if (chosen == remove) {
    mira_gui::actions::BatchDelete(this, named, [this] { RefreshGames(); });
  }
}

void LibraryWindow::BatchHide(const std::vector<std::string>& ids) {
  for (const std::string& id : ids) {
    const mira_gui::GameSummary* game = FindGame(id);
    if (game == nullptr || HasTag(*game, "hidden")) continue;

    std::vector<std::string> tags = game->tags;
    tags.push_back("hidden");
    mira_gui::GamePatch patch;
    patch.tags = tags;
    mira_gui::MiradClient::PatchGameAsync(
        this, id, patch, [this, id, tags](mira_gui::PatchGameResult result) {
          if (!result.ok) {
            mira_gui::notify::Failed(this, "Could not hide a game.",
                                     QString::fromStdString(result.error));
            return;
          }
          for (mira_gui::GameSummary& stored : games_) {
            if (stored.id == id) {
              stored.tags = tags;
              break;
            }
          }
          ApplyFilter();
        });
  }
}

void LibraryWindow::ToggleHidden(const std::string& id) {
  const mira_gui::GameSummary* game = FindGame(id);
  if (game == nullptr) return;

  std::vector<std::string> tags = game->tags;
  const bool was_hidden = HasTag(*game, "hidden");
  if (was_hidden) {
    tags.erase(std::remove(tags.begin(), tags.end(), "hidden"), tags.end());
  } else {
    tags.push_back("hidden");
  }

  mira_gui::GamePatch patch;
  patch.tags = tags;
  mira_gui::MiradClient::PatchGameAsync(this, id, patch, [this, id, tags](mira_gui::PatchGameResult result) {
    if (!result.ok) {
      mira_gui::notify::Failed(this, "Could not change this game's visibility.",
                               QString::fromStdString(result.error));
      return;
    }
    // Patched in place rather than waiting for the game.updated event, so
    // Hide/Unhide feels instant. No toast: the game already visibly
    // vanishing from (or appearing in) the grid is the feedback -- a
    // notification on top of that would just be noise.
    for (mira_gui::GameSummary& stored : games_) {
      if (stored.id == id) {
        stored.tags = tags;
        break;
      }
    }
    ApplyFilter();
  });
}

void LibraryWindow::ToggleRunning(const std::string& id) {
  if (running_ids_.contains(id)) {
    mira_gui::actions::Stop(this, id);
  } else {
    LaunchGame(id);
  }
}

void LibraryWindow::LaunchGame(const std::string& id) {
  mira_gui::actions::Launch(this, id, [this, id](bool tracked) {
    // Only a tracked launch gets a "Playing now" tile. A Steam-launched game
    // never emits game.state, so marking it running here would pin it with
    // no event to release it.
    if (tracked) running_ids_.insert(id);
    RefreshGames();
  });
}

void LibraryWindow::OpenGameDialog(const std::string& id) {
  if (!game_settings_in_sidebar_) {
    GameDetailDialog dialog(id, this, artwork_);
    dialog.exec();
    RefreshGames();
    return;
  }

  // Fresh instance each time: GameEditForm loads its id at construction.
  if (game_edit_card_ != nullptr) {
    game_edit_overlay_layout_->removeWidget(game_edit_card_);
    game_edit_card_->deleteLater();
  }
  SetGridControlsEnabled(false);
  game_edit_card_ = BuildGameEditCard(id);
  game_edit_overlay_layout_->addWidget(game_edit_card_, 0, 0, Qt::AlignCenter);
  root_stack_->setCurrentWidget(game_edit_overlay_);
  game_edit_overlay_->show();
  UpdateLibraryNavActive();
}

void LibraryWindow::CloseGameEdit() {
  game_edit_overlay_->hide();
  // Index 0 is chrome (root_stack_ only ever holds these two) -- raising it
  // back on top is cosmetic once the overlay is hidden, but keeps z-order
  // consistent for the next OpenGameDialog.
  root_stack_->setCurrentIndex(0);
  SetGridControlsEnabled(true);
  UpdateLibraryNavActive();
  // Torn down rather than left alive off-screen: IsDirty() on a discarded
  // form would otherwise still read dirty, and wrongly prompt again on the
  // next Ctrl+Q from the grid.
  if (game_edit_card_ != nullptr) {
    game_edit_overlay_layout_->removeWidget(game_edit_card_);
    game_edit_card_->deleteLater();
    game_edit_card_ = nullptr;
    game_edit_form_ = nullptr;
  }
  RefreshGames();
}

void LibraryWindow::RequestCloseGameEdit() {
  if (game_edit_form_ == nullptr || !game_edit_form_->IsDirty()) {
    CloseGameEdit();
    return;
  }
  switch (mira_gui::notify::ConfirmUnsaved(this, "This game's edits aren't saved.")) {
    case mira_gui::notify::UnsavedAction::Cancel:
      return;
    case mira_gui::notify::UnsavedAction::SaveAndExit:
      game_edit_form_->Save();  // SaveFinished, connected in BuildGameEditCard, closes on success
      return;
    case mira_gui::notify::UnsavedAction::DiscardAndExit:
      CloseGameEdit();
      return;
  }
}

bool LibraryWindow::GameEditOpen() const {
  return game_edit_overlay_ != nullptr && game_edit_overlay_->isVisible();
}

void LibraryWindow::OpenSettings(const QString& focus_key) {
  // Already open: rebuilding would throw away whatever is half-typed.
  if (SettingsOpen()) {
    if (!focus_key.isEmpty() && settings_panel_ != nullptr) settings_panel_->FocusKey(focus_key);
    return;
  }

  // Fresh instance each time: starts synced to what's actually saved,
  // not stale edits left in the widgets from a discarded previous open.
  if (settings_page_ != nullptr) {
    content_stack_->removeWidget(settings_page_);
    settings_page_->deleteLater();
  }
  settings_page_ = BuildSettingsPage();
  content_stack_->addWidget(settings_page_);
  content_stack_->setCurrentWidget(settings_page_);
  SetSettingsChromeVisible(true);
  if (!focus_key.isEmpty()) settings_panel_->FocusKey(focus_key);
}

void LibraryWindow::CloseSettings() {
  content_stack_->setCurrentWidget(splitter_);
  SetSettingsChromeVisible(false);
  // Torn down rather than left alive off-screen: IsDirty() on a discarded
  // panel would otherwise still read dirty, and wrongly prompt again on the
  // next Ctrl+Q from the grid.
  if (settings_page_ != nullptr) {
    content_stack_->removeWidget(settings_page_);
    settings_page_->deleteLater();
    settings_page_ = nullptr;
    settings_panel_ = nullptr;
  }
}

bool LibraryWindow::SettingsOpen() const {
  return settings_page_ != nullptr && content_stack_->currentWidget() == settings_page_;
}

void LibraryWindow::SetSettingsChromeVisible(bool settings_open) {
  SetGridControlsEnabled(!settings_open);
  for (QPushButton* nav : source_navs_) nav->setEnabled(!settings_open);
  UpdateLibraryNavActive();
}

void LibraryWindow::SetGridControlsEnabled(bool enabled) {
  // The grid itself is what's leaving the screen either way -- nothing left
  // to preview.
  if (!enabled) ShowHoverCard(nullptr);
  // These act on a hidden grid. library_nav_ stays clickable — it's the way
  // back out. Disabling filter_sort_button_ alone blocks its popover too.
  for (QWidget* control :
       {filter_sort_button_, static_cast<QWidget*>(add_games_), static_cast<QWidget*>(search_),
        static_cast<QWidget*>(zoom_), static_cast<QWidget*>(settings_button_),
        static_cast<QWidget*>(classic_view_nav_)}) {
    control->setEnabled(enabled);
  }
  // Back from Settings onto a source page: the grid is still covered.
  if (enabled && source_page_ != nullptr) SetSourceControlsEnabled(false);
}

void LibraryWindow::UpdateLibraryNavActive() {
  using mira_gui::icons::Glyph;
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();

  const bool library_active = GridShown() && !GameEditOpen();
  if (library_nav_ != nullptr) {
    library_nav_->setChecked(library_active);
    library_nav_->setIcon(
        mira_gui::icons::For(Glyph::Home, library_active ? tokens.on_accent : tokens.text));
  }
  const bool classic_active = content_stack_->currentWidget() == classic_page_;
  if (classic_view_nav_ != nullptr) {
    classic_view_nav_->setChecked(classic_active);
    classic_view_nav_->setIcon(
        mira_gui::icons::For(Glyph::Table, classic_active ? tokens.on_accent : tokens.text));
  }
  const QString open_source = content_stack_->currentWidget() == splitter_ && source_page_ != nullptr
                                  ? source_page_->property("source_id").toString()
                                  : QString();
  const std::vector<mira_gui::SourceInfo>& sources = mira_gui::AllSources();
  for (int i = 0; i < source_navs_.size() && i < static_cast<int>(sources.size()); ++i) {
    const bool active = sources[i].id == open_source;
    source_navs_[i]->setChecked(active);
    source_navs_[i]->setIcon(
        mira_gui::icons::For(Glyph::Store, active ? tokens.on_accent : tokens.text));
  }
}

void LibraryWindow::RequestCloseSettings() {
  if (settings_panel_ == nullptr || !settings_panel_->IsDirty()) {
    CloseSettings();
    return;
  }
  switch (mira_gui::notify::ConfirmUnsaved(this, "Settings changed but not saved.")) {
    case mira_gui::notify::UnsavedAction::Cancel:
      return;
    case mira_gui::notify::UnsavedAction::SaveAndExit:
      settings_panel_->Save();  // SaveFinished, connected in BuildSettingsPage, closes on success
      return;
    case mira_gui::notify::UnsavedAction::DiscardAndExit:
      CloseSettings();
      return;
  }
}

QWidget* LibraryWindow::BuildSettingsPage() {
  auto* page = new QWidget(this);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 12, 16, 16);
  layout->setSpacing(10);

  auto* title = new QLabel("Settings", page);
  title->setProperty("role", "heading");
  layout->addWidget(title);

  settings_panel_ = new mira_gui::SettingsPanel(page);
  connect(settings_panel_, &mira_gui::SettingsPanel::LoadFailed, this, [this](QString error) {
    mira_gui::notify::Failed(this, "Could not load the settings.", error);
    CloseSettings();
  });
  connect(settings_panel_, &mira_gui::SettingsPanel::SaveFinished, this,
          [this](bool ok, QString error) {
            if (!ok) {
              mira_gui::notify::Failed(this, "Could not save the settings.", error);
              return;
            }
            // The screen closing back to the grid is already the feedback —
            // a save the user just triggered isn't the background-result
            // case a toast is for.
            CloseSettings();
            // Picks up a changed game_settings_in_sidebar without a restart.
            LoadPrefs();
            RefreshSourceNavs();
          });
  layout->addWidget(settings_panel_, /*stretch=*/1);

  // Pinned under the settings nav's category list.
  auto* actions = new QWidget();
  auto* actions_layout = new QHBoxLayout(actions);
  actions_layout->setContentsMargins(0, 0, 0, 0);
  actions_layout->setSpacing(6);
  auto* back = new QPushButton("← Back", actions);
  connect(back, &QPushButton::clicked, this, &LibraryWindow::RequestCloseSettings);
  auto* reset = new QPushButton("Reset", actions);
  reset->setToolTip("Discard unsaved changes on this screen — back to what was last saved.");
  connect(reset, &QPushButton::clicked, this, [this] {
    if (settings_panel_ != nullptr) settings_panel_->DiscardChanges();
  });
  auto* save = new QPushButton("Save", actions);
  connect(save, &QPushButton::clicked, this, [this] {
    if (settings_panel_ != nullptr) settings_panel_->Save();
  });
  actions_layout->addWidget(back);
  actions_layout->addWidget(reset);
  actions_layout->addWidget(save);
  settings_panel_->SetFooterActions(actions);

  return page;
}

QWidget* LibraryWindow::BuildGameEditOverlay() {
  // Parented to nullptr here -- root_stack_->addWidget(overlay) reparents it
  // to central, same as any other widget added to a layout.
  auto* overlay = new ModalOverlay(nullptr);
  overlay->setObjectName("game_edit_overlay");
  // Plain black, not theme::window -- the theme's dark surfaces already
  // sit close to black, so tinting toward window barely dims anything.
  QColor scrim(0, 0, 0, 150);
  overlay->setStyleSheet(
      QString("QWidget#game_edit_overlay { background: rgba(%1, %2, %3, %4); }")
          .arg(scrim.red())
          .arg(scrim.green())
          .arg(scrim.blue())
          .arg(scrim.alpha()));
  overlay->hide();
  overlay->on_backdrop_clicked = [this] { RequestCloseGameEdit(); };

  game_edit_overlay_layout_ = new QGridLayout(overlay);
  game_edit_overlay_layout_->setContentsMargins(24, 24, 24, 24);
  return overlay;
}

QWidget* LibraryWindow::BuildGameEditCard(const std::string& id) {
  auto* card = new QWidget();
  card->setObjectName("game_edit_card");
  // ~70% of the window, not a hardcoded constant -- recomputed per open
  // since the window can resize between edits.
  card->setFixedSize(qRound(width() * 0.7), qRound(height() * 0.7));

  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // Header: name + status, static (the scrollable form below has its own
  // editable Name field) -- just enough identity to confirm which game this
  // is, plus the close affordance an overlay needs beyond the scrim click.
  auto* header = new QWidget(card);
  auto* header_layout = new QHBoxLayout(header);
  header_layout->setContentsMargins(18, 14, 10, 14);
  const mira_gui::GameSummary* game = FindGame(id);
  auto* title = new QLabel(game != nullptr ? QString::fromStdString(game->name) : "Game settings",
                           header);
  title->setProperty("role", "heading");
  header_layout->addWidget(title, /*stretch=*/1);
  auto* close = new QToolButton(header);
  close->setAutoRaise(true);
  close->setIcon(mira_gui::icons::For(mira_gui::icons::Glyph::Close));
  close->setToolTip("Close");
  connect(close, &QToolButton::clicked, this, &LibraryWindow::RequestCloseGameEdit);
  header_layout->addWidget(close);
  layout->addWidget(header);

  auto* divider = new QWidget(card);
  divider->setFixedHeight(1);
  divider->setStyleSheet(QString("background: %1;").arg(mira_gui::theme::Current().border.name()));
  layout->addWidget(divider);

  auto* scroll = new QScrollArea(card);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  // GameEditForm has no margins of its own; pad it here, 16px to match
  // GameDetailDialog.
  auto* form_container = new QWidget();
  auto* form_container_layout = new QVBoxLayout(form_container);
  form_container_layout->setContentsMargins(16, 16, 16, 16);
  game_edit_form_ = new mira_gui::GameEditForm(id, form_container);
  game_edit_form_->SetArtworkStore(artwork_);
  form_container_layout->addWidget(game_edit_form_);
  connect(game_edit_form_, &mira_gui::GameEditForm::ArtworkPickRequested, this,
          [this, id](const QString& slot) { OpenArtworkPicker(id, slot.toStdString()); });
  connect(game_edit_form_, &mira_gui::GameEditForm::LoadFailed, this, [this](QString error) {
    mira_gui::notify::Failed(this, "Could not load this game.", error);
    CloseGameEdit();
  });
  connect(game_edit_form_, &mira_gui::GameEditForm::SaveFinished, this,
          [this](bool ok, QString error) {
            if (!ok) {
              mira_gui::notify::Failed(this, "Could not save this game.", error);
              return;
            }
            // The overlay closing back to the grid is already the feedback —
            // a save the user just triggered isn't the background-result
            // case a toast is for.
            CloseGameEdit();
          });
  scroll->setWidget(form_container);
  layout->addWidget(scroll, /*stretch=*/1);

  auto* footer = new QWidget(card);
  auto* footer_layout = new QHBoxLayout(footer);
  footer_layout->setContentsMargins(14, 12, 14, 14);
  auto* back = new QPushButton("← Back", footer);
  connect(back, &QPushButton::clicked, this, &LibraryWindow::RequestCloseGameEdit);
  auto* save = new QPushButton("Save", footer);
  connect(save, &QPushButton::clicked, game_edit_form_, &mira_gui::GameEditForm::Save);
  footer_layout->addWidget(back);
  footer_layout->addStretch(1);
  footer_layout->addWidget(save);
  layout->addWidget(footer);

  return card;
}

void LibraryWindow::OpenSource(const mira_gui::SourceInfo& source) {
  if (content_stack_->currentWidget() == classic_page_) CloseClassicView();
  if (source_page_ != nullptr) {
    main_stack_->removeWidget(source_page_);
    source_page_->deleteLater();
  }
  source_page_ = new mira_gui::SourcePage(source, artwork_, this);
  source_page_->SetGames(games_, running_ids_);
  source_page_->setProperty("source_id", source.id);
  connect(source_page_, &mira_gui::SourcePage::BackRequested, this, &LibraryWindow::CloseSource);
  connect(source_page_, &mira_gui::SourcePage::LibraryChanged, this, &LibraryWindow::RefreshGames);
  connect(source_page_, &mira_gui::SourcePage::OpenSettingsRequested, this,
          [this](const QString& key) { OpenSettings(key); });
  connect(source_page_, &mira_gui::SourcePage::OpenGameRequested, this,
          [this](const QString& id) { OpenGameDialog(id.toStdString()); });
  // Same rule as the grid's double-click: only a ready game has anything to launch.
  connect(source_page_, &mira_gui::SourcePage::PlayRequested, this, [this](const QString& id) {
    const mira_gui::GameSummary* game = FindGame(id.toStdString());
    if (game == nullptr) return;
    if (running_ids_.contains(game->id) || game->status == "ready") ToggleRunning(game->id);
  });
  main_stack_->addWidget(source_page_);
  main_stack_->setCurrentWidget(source_page_);
  SetSourceControlsEnabled(false);
  UpdateLibraryNavActive();
}

void LibraryWindow::CloseSource() {
  main_stack_->setCurrentWidget(grid_page_);
  if (source_page_ != nullptr) {
    main_stack_->removeWidget(source_page_);
    source_page_->deleteLater();
    source_page_ = nullptr;
  }
  SetSourceControlsEnabled(true);
  UpdateLibraryNavActive();
}

// Only what acts on the grid; the rest of the sidebar stays usable.
void LibraryWindow::SetSourceControlsEnabled(bool enabled) {
  if (!enabled) ShowHoverCard(nullptr);
  for (QWidget* control : {filter_sort_button_, static_cast<QWidget*>(search_), static_cast<QWidget*>(zoom_)}) {
    control->setEnabled(enabled);
  }
}

bool LibraryWindow::GridShown() const {
  return content_stack_->currentWidget() == splitter_ && main_stack_->currentWidget() == grid_page_;
}

void LibraryWindow::RefreshSourceNavs() {
  mira_gui::MiradClient::GetConfigAsync(this, [this](mira_gui::ConfigResult result) {
    if (!result.ok) return;  // every source stays listed
    const std::vector<mira_gui::SourceInfo>& sources = mira_gui::AllSources();
    for (int i = 0; i < source_navs_.size() && i < static_cast<int>(sources.size()); ++i) {
      const auto found = result.values.find(sources[i].id.toStdString() + ".enabled");
      source_navs_[i]->setVisible(found == result.values.end() || found->second != "false");
    }
  });
}

void LibraryWindow::OpenClassicView() {
  if (source_page_ != nullptr) CloseSource();
  content_stack_->setCurrentWidget(classic_page_);
  SetGridControlsEnabled(false);
  UpdateLibraryNavActive();
}

void LibraryWindow::CloseClassicView() {
  content_stack_->setCurrentWidget(splitter_);
  SetGridControlsEnabled(true);
  UpdateLibraryNavActive();
}

QWidget* LibraryWindow::BuildClassicPage() {
  auto* page = new QWidget(this);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 12, 16, 16);
  layout->setSpacing(10);

  auto* header = new QHBoxLayout();
  auto* back = new QPushButton("← Back", page);
  connect(back, &QPushButton::clicked, this, &LibraryWindow::CloseClassicView);
  auto* title = new QLabel("Classic table view", page);
  title->setProperty("role", "heading");
  header->addWidget(back);
  header->addWidget(title);
  header->addStretch(1);
  layout->addLayout(header);

  classic_table_ = new QTableWidget(0, 7, page);
  classic_table_->setHorizontalHeaderLabels(
      {"Name", "Status", "Platform", "Runner", "Last Played", "Playtime", ""});
  classic_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  classic_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  classic_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  classic_table_->setAlternatingRowColors(true);
  classic_table_->verticalHeader()->setVisible(false);
  classic_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int column = 1; column <= 6; ++column) {
    classic_table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
  }
  classic_table_->setShowGrid(false);
  connect(classic_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
    QTableWidgetItem* item = classic_table_->item(row, 0);
    if (item != nullptr) OpenGameDialog(item->data(Qt::UserRole).toString().toStdString());
  });
  layout->addWidget(classic_table_, /*stretch=*/1);

  return page;
}

void LibraryWindow::RefreshClassicTable() {
  if (classic_table_ == nullptr) return;

  // Same source and same filter as the grid — one games_ list, two
  // presentations, always in sync since both are rebuilt from ApplyFilter.
  std::vector<const mira_gui::GameSummary*> shown;
  for (const mira_gui::GameSummary& game : games_) {
    if (MatchesFilter(game)) shown.push_back(&game);
  }

  // Sorting stays off for the whole repopulate: Qt re-sorts on every setItem
  // to the sort column, which would move a row out from under the loop
  // that's still filling its other columns.
  classic_table_->setSortingEnabled(false);
  classic_table_->setRowCount(static_cast<int>(shown.size()));
  for (int row = 0; row < static_cast<int>(shown.size()); ++row) {
    const mira_gui::GameSummary& game = *shown[row];

    auto* name_item = new QTableWidgetItem(QString::fromStdString(game.name));
    name_item->setData(Qt::UserRole, QString::fromStdString(game.id));

    auto* status_item = new QTableWidgetItem(QString::fromStdString(game.status));
    status_item->setForeground(mira_gui::StatusColor(game.status));
    if (!game.last_error.empty()) status_item->setToolTip(QString::fromStdString(game.last_error));

    auto* platform_item = new QTableWidgetItem(QString::fromStdString(game.platform));
    auto* runner_item = new QTableWidgetItem(
        game.runner_ref.empty() ? "Auto" : QString::fromStdString(game.runner_ref));
    auto* last_played_item = new QTableWidgetItem(mira_gui::FormatLastPlayed(game.last_played_at));
    auto* playtime_item = new QTableWidgetItem(mira_gui::FormatPlaytime(game.play_seconds));

    classic_table_->setItem(row, 0, name_item);
    classic_table_->setItem(row, 1, status_item);
    classic_table_->setItem(row, 2, platform_item);
    classic_table_->setItem(row, 3, runner_item);
    classic_table_->setItem(row, 4, last_played_item);
    classic_table_->setItem(row, 5, playtime_item);

    auto* actions_widget = new QWidget(classic_table_);
    auto* actions_layout = new QHBoxLayout(actions_widget);
    actions_layout->setContentsMargins(0, 0, 0, 0);
    actions_layout->setSpacing(4);

    const std::string id = game.id;
    const QString name = QString::fromStdString(game.name);
    const bool running = running_ids_.contains(id);

    auto* launch_button = new QPushButton(running ? "Stop" : "Launch", classic_table_);
    const bool can_launch = game.status == "ready";
    launch_button->setEnabled(running || can_launch);
    if (!running && !can_launch) {
      launch_button->setToolTip(
          QString("Not launchable while %1").arg(QString::fromStdString(game.status)));
    }
    connect(launch_button, &QPushButton::clicked, this, [this, id] { ToggleRunning(id); });
    actions_layout->addWidget(launch_button);

    auto* delete_button = new QPushButton("Delete", classic_table_);
    connect(delete_button, &QPushButton::clicked, this, [this, id, name] {
      mira_gui::actions::Delete(this, id, name, [this] { RefreshGames(); });
    });
    actions_layout->addWidget(delete_button);

    classic_table_->setCellWidget(row, 6, actions_widget);
  }
  classic_table_->setSortingEnabled(true);
}

void LibraryWindow::HandleGameEvent(const std::string& type, const std::string& data) {
  if (type == "notification") {
    mira_gui::NotificationEvent event;
    if (mira_gui::MiradClient::ParseNotification(data, &event)) {
      const QString message = QString::fromStdString(event.message);
      const auto level = mira_gui::notify::LevelFromString(QString::fromStdString(event.level));
      if (level == mira_gui::notify::Level::Warning || level == mira_gui::notify::Level::Error) {
        mira_gui::notify::Warn(this, message);
      } else {
        mira_gui::notify::Notice(this, message);
      }
    }
    return;
  }

  if (type == "game.removed") {
    const std::string id = mira_gui::MiradClient::ParseRemovedId(data);
    if (!id.empty()) RemoveGame(id);
    return;
  }

  if (type == "game.state") {
    mira_gui::GameStateEvent state;
    if (!mira_gui::MiradClient::ParseGameState(data, &state)) return;
    if (state.state == "running") {
      running_ids_.insert(state.id);
    } else {
      running_ids_.erase(state.id);
    }
    // Carries the full record now, so patch the row instead of relisting.
    mira_gui::GameSummary game;
    if (mira_gui::MiradClient::ParseGameSummary(data, &game)) {
      UpsertGame(game);
    } else {
      RefreshGames();
    }
    return;
  }

  if (type == "game.metadata_ready" || type == "game.metadata_failed") {
    mira_gui::MetadataEvent event;
    if (!mira_gui::MiradClient::ParseMetadataEvent(data, &event)) return;
    if (type == "game.metadata_ready") {
      // Only the artwork is refetched here — the rest of the metadata isn't
      // part of the game record, so nothing else in the library view changes.
      artwork_->Invalidate(event.id);
      return;
    }
    // The one failure worth interrupting for: it's fixable and never
    // transient — no SteamGridDB key means every non-Steam game keeps its
    // placeholder forever.
    if (event.code == "no_steamgriddb_key") {
      const bool asked_for = awaiting_metadata_.erase(event.id) > 0 || artwork_fetch_requested_;
      ShowSteamGridDbNotice(asked_for);
      return;
    }

    // Everything else mirad already reports as a `notification` event when
    // the fetch was announced.
    awaiting_metadata_.erase(event.id);
    return;
  }

  if (type == "game.artwork_selected") {
    mira_gui::ArtworkSelectEvent event;
    if (!mira_gui::MiradClient::ParseArtworkSelectEvent(data, &event)) return;
    if (event.slot == "cover") {
      artwork_->Invalidate(event.id);
    } else if (event.slot == "hero") {
      if (game_edit_form_ != nullptr) game_edit_form_->RefreshBanner(event.id);
    }
    return;
  }

  if (type == "game.launched") {
    // mirad hands a Steam game to steam://rungameid and says whether it is
    // watching the process. Tracked: leave it alone, game.state is coming.
    // Untracked: clear it, since nothing will ever say it stopped.
    mira_gui::GameLaunchedEvent launched;
    if (mira_gui::MiradClient::ParseGameLaunched(data, &launched) && !launched.tracked) {
      running_ids_.erase(launched.id);
      RefreshGames();
    }
    return;
  }

  // Explicitly the two event types that carry a game record, not "anything
  // left over" — mirad also publishes runners.download.* and tricks.* here.
  if (type != "game.added" && type != "game.updated") return;

  mira_gui::GameSummary game;
  if (mira_gui::MiradClient::ParseGameSummary(data, &game)) UpsertGame(game);
}
