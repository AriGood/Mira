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
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QCloseEvent>
#include <QDrag>
#include <QMimeData>
#include <QApplication>
#include <QDropEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QPushButton>
#include <QRubberBand>
#include <QScreen>
#include <QScrollBar>
#include <QItemSelection>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
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
#include "../dialogs/DesktopEntryImportDialog.h"
#include "../dialogs/GameDetailDialog.h"
#include "../dialogs/GameDetailPageDialog.h"
#include "../dialogs/ManageSourcesDialog.h"

#include "../ui/AboutPanel.h"
#include "../ui/CoverArt.h"
#include "../ui/DaemonSupervisor.h"
#include "../ui/DownloadTracker.h"
#include "../ui/DownloadsPanel.h"
#include "../ui/GameActions.h"
#include "../ui/GameEditForm.h"
#include "../ui/GamePresentation.h"
#include "../ui/GameTileDelegate.h"
#include "../ui/ArtPickerPanel.h"
#include "../ui/HeroBackdrop.h"
#include "../ui/HoverCard.h"
#include "../ui/Icons.h"
#include "../ui/KeyBindings.h"
#include "../ui/LibrarySort.h"
#include "../ui/Notify.h"
#include "../ui/ContinueRow.h"
#include "../ui/TabRow.h"
#include "../ui/Sources.h"
#include "../ui/SettingsPanel.h"
#include "../ui/Shortcuts.h"
#include "../ui/Theme.h"
#include "../ui/TileView.h"
#include "../ui/Tray.h"
#include "RunnersPage.h"
#include "SourcePage.h"

// setViewportMargins is protected on QAbstractScrollArea; this republishes
// it so ApplyLayoutTokens() can pad the tiles without also insetting the
// scrollbar. Ctrl+wheel resizes tiles instead of scrolling.
class LibraryGrid : public mira_gui::TileView {
public:
  using TileView::TileView;
  using TileView::setViewportMargins;

  // One call per notch, positive to grow. Set once by LibraryWindow.
  std::function<void(int steps)> on_ctrl_wheel;

protected:
  void wheelEvent(QWheelEvent* event) override {
    if (event->modifiers() & Qt::ControlModifier) {
      // angleDelta() is in eighths of a degree; a notch is 15 degrees (120).
      const int steps = event->angleDelta().y() / 120;
      if (steps != 0 && on_ctrl_wheel) on_ctrl_wheel(steps);
      event->accept();
      return;
    }
    TileView::wheelEvent(event);
  }
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
    // After Hidden so Ctrl+1…9 keep their filters.
    {"Needs attention", "attention", mira_gui::icons::Glyph::Warning},
};

// The filters the library's tab row offers, with its own shorter labels.
const std::pair<const char*, const char*> kFilterTabs[] = {
    {"all", "All"},           {"ready", "Installed"},  {"running", "Playing now"},
    {"attention", "Needs attention"}, {"never", "Never played"},
};

// A crash within this many seconds of launch reads as "failed to start".
constexpr std::int64_t kFailedStartSeconds = 30;

// A pinned game's tag. "favorite" because Lutris imports its favorites under it.
constexpr const char* kPinnedTag = "favorite";

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
    QWindow* handle = window_->windowHandle();
    if (event->button() == Qt::LeftButton && !window_->isMaximized() && handle != nullptr) {
      const Qt::Edges edges = ResizableEdgesAt(event->pos());
      if (edges != Qt::Edges()) {
        handle->startSystemResize(edges);
        event->accept();
        return;
      }
      // The strip above the top bar moves the window like the bar does.
      if (event->pos().y() <= kResizeMargin) {
        handle->startSystemMove();
        event->accept();
        return;
      }
    }
    QWidget::mousePressEvent(event);
  }

  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->pos().y() <= kResizeMargin && ResizableEdgesAt(event->pos()) == Qt::Edges()) {
      window_->isMaximized() ? window_->showNormal() : window_->showMaximized();
      return;
    }
    QWidget::mouseDoubleClickEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (window_->isMaximized()) {
      unsetCursor();
      return;
    }
    const Qt::Edges edges = ResizableEdgesAt(event->pos());
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
  // Edges under `pos` that resize. The top edge only resizes at its
  // corners; the rest of it moves the window, since a drag up there (often
  // toward the top of the screen, e.g. out of a tiled corner) is meant to
  // move it. Wayland doesn't tell a window where it is, so this can't
  // depend on the screen edges.
  Qt::Edges ResizableEdgesAt(const QPoint& pos) const {
    constexpr int kCorner = 14;
    Qt::Edges edges = EdgesAt(size(), pos);
    if (!(edges & Qt::TopEdge)) return edges;
    if (pos.x() <= kCorner) return Qt::TopEdge | Qt::LeftEdge;
    if (pos.x() >= width() - kCorner) return Qt::TopEdge | Qt::RightEdge;
    return edges & ~Qt::Edges(Qt::TopEdge);
  }

  QMainWindow* window_;
};

// Sidebar's filter+sort pill. Plain QWidget, not QPushButton: needs two
// icon+label pairs and a chevron, not one icon+text. Plain callback (like
// LibraryGrid), not a signal, since it is too small to need one.
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
  // A press on the card's own empty space propagates up to here too, so
  // only one that lands on no child at all counts as the backdrop.
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() != Qt::LeftButton || !on_backdrop_clicked) return;
    if (childAt(event->position().toPoint()) != nullptr) return;
    on_backdrop_clicked();
  }
};

QLabel* SidebarHeading(QWidget* parent, const QString& text) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", "muted");
  label->setStyleSheet("font-weight: 600; letter-spacing: 0.04em; margin-top: 14px; margin-bottom: 2px;");
  return label;
}

// A muted label on the row's right, e.g. a game count.
QLabel* AddTrailingLabel(QPushButton* row) {
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 10, 0);
  layout->addStretch(1);
  auto* label = new QLabel(row);
  label->setProperty("role", "muted");
  label->setAttribute(Qt::WA_TransparentForMouseEvents);
  layout->addWidget(label);
  return label;
}

}  // namespace

LibraryWindow::LibraryWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("Mira");
  // Sized and centered before the first show: resizing once shown grows the
  // window from its top-left corner, off center.
  QSize size(1180, 720);
  const mira_gui::FrontendPrefsResult saved = mira_gui::MiradClient::GetFrontendPrefsBlocking();
  if (saved.ok && saved.prefs.window_width && saved.prefs.window_height) {
    size = QSize(*saved.prefs.window_width, *saved.prefs.window_height);
  }
  if (const QScreen* screen = QGuiApplication::primaryScreen()) {
    const QRect available = screen->availableGeometry();
    size = size.boundedTo(available.size());
    setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size, available));
  } else {
    resize(size);
  }
  // Custom top bar takes over move/resize/minimize/maximize/close, so no OS
  // decoration left.
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);

  // Before the panel and the grid, because both ask it for covers.
  artwork_ = new mira_gui::ArtworkStore(this);
  connect(artwork_, &mira_gui::ArtworkStore::CoverChanged, this, &LibraryWindow::UpdateTileCover);

  InstallErrorNavigator();

  // Before the top bar, which shows its count.
  downloads_ = new mira_gui::DownloadTracker(this);
  downloads_->game_name = [this](const std::string& id) {
    const mira_gui::GameSummary* game = FindGame(id);
    return game != nullptr ? QString::fromStdString(game->name) : QString();
  };
  downloads_->source_name = [](const QString& id) {
    for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
      if (source.id == id) return source.name;
    }
    return id;
  };
  connect(downloads_, &mira_gui::DownloadTracker::Changed, this, &LibraryWindow::DownloadChanged);
  downloads_panel_ = new mira_gui::DownloadsPanel(downloads_, artwork_, this);
  connect(downloads_panel_, &mira_gui::DownloadsPanel::ShowGameRequested, this,
          [this](const QString& id) { ShowGame(id.toStdString()); });

  // The stylesheet re-polishes every widget by itself; what it cannot reach
  // is what we paint: the tiles, and the placeholder covers drawn in the
  // theme's own colors.
  connect(mira_gui::theme::Notifier::Instance(), &mira_gui::theme::Notifier::Changed, this, [this] {
    artwork_->InvalidateAllRenderings();
    ApplyLayoutTokens();
    ApplyFilter();
    ApplyTopBarIcons();
    UpdateLibraryNavActive();  // the checked rows' icons are on_accent
  });

  splitter_ = new QSplitter(Qt::Horizontal, this);
  splitter_->addWidget(BuildSidebar());
  // A source page or the classic table takes the grid's place here, leaving
  // the sidebar up.
  main_stack_ = new QStackedWidget(this);
  grid_page_ = BuildGrid();
  main_stack_->addWidget(grid_page_);
  classic_page_ = BuildClassicPage();
  main_stack_->addWidget(classic_page_);
  splitter_->addWidget(main_stack_);
  splitter_->setStretchFactor(0, 0);
  splitter_->setStretchFactor(1, 1);
  splitter_->setSizes({232, 850});
  splitter_->setChildrenCollapsible(false);

  // Settings is built lazily by OpenSettings() and covers this slot. A
  // game's edit card is a separate overlay, not a page here.
  content_stack_ = new QStackedWidget(this);
  content_stack_->addWidget(splitter_);

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

  BuildShortcuts();
  UpdateLibraryNavActive();

  LoadPrefs();
  RefreshSourceNavs();
  RefreshHealth(/*force_scan=*/false);

  event_stream_.Start(this,
                      [this](std::string type, std::string data) { HandleGameEvent(type, data); });
}

void LibraryWindow::BuildShortcuts() {
  common_ = mira_gui::shortcuts::Install(
      this, {
                {"Ctrl+F", "Focus the search box"},
                {"Esc", "Clear the search, then the selection"},
                {"Ctrl+1…9", "Pick a filter"},
                {"Ctrl+H", "Toggle the Hidden filter"},
                {"F5, Ctrl+R", "Refresh the library"},
                {"Enter", "Play the selected game, or stop it while it runs"},
                {"Alt+Enter", "Game settings"},
                {"Delete", "Remove the selected game"},
                {"Ctrl++, Ctrl+-", "Tile size"},
                {"Ctrl+0", "Reset tile size"},
                {"Ctrl+,", "Settings"},
            });

  // Each of these registers with ui/KeyBindings so Settings' Shortcuts
  // category can list and edit it; Ctrl+1…9's per-filter loop below is the
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
      if (ArtPickerOpen()) {
        CloseArtPicker();
      } else {
        RequestCloseGameEdit();
      }
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
  // registered with keybindings: nine near-identical rebindable rows for
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

  // Neither is a menu entry anymore (both are sidebar rows now), kept here
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

  // Qt::Key_Enter is the keypad one, a separate key from Qt::Key_Return,
  // and binding only Return would leave it dead.
  grid_action("play_stop", "Play the selected game, or stop it while it runs", QKeySequence(Qt::Key_Return),
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
      UpdateFilterSortSummary();
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
    // Not Hidden: opening on hidden games reads as the library being gone.
    if (prefs.library_filter && *prefs.library_filter != "hidden") {
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
    if (prefs.hidden_sources) {
      hidden_sources_.clear();
      for (const std::string& id : *prefs.hidden_sources) hidden_sources_.insert(QString::fromStdString(id));
    }
    if (prefs.source_imported_at) {
      source_imported_at_.clear();
      for (const auto& [id, at] : *prefs.source_imported_at) source_imported_at_[QString::fromStdString(id)] = at;
    }
    if (prefs.source_order) {
      source_order_.clear();
      for (const std::string& id : *prefs.source_order) source_order_.push_back(QString::fromStdString(id));
    }
    recent_count_ = prefs.sidebar_recent_count.value_or(0);
    show_source_counts_ = prefs.sidebar_source_counts.value_or(true);
    source_icons_ = prefs.sidebar_source_icons.value_or(true);
    library_tabs_->SetTabsVisible(prefs.library_filter_tabs.value_or(true));
    continue_row_enabled_ = prefs.library_continue_row.value_or(true);
    continue_count_ = prefs.library_continue_count.value_or(3);
    source_page_tabs_ = prefs.source_page_tabs.value_or(true);
    tile_size_synced_ = prefs.tile_size_synced.value_or(false);
    source_tile_widths_ = prefs.source_tile_widths.value_or(std::map<std::string, int>{});
    delegate_->SetShowStatus(prefs.tile_status.value_or(true));
    delegate_->SetShowSourceMark(prefs.tile_source_mark.value_or(true));
    drag_select_ = prefs.drag_select.value_or(true);
    grid_->SetDragSelectEnabled(drag_select_);
    if (source_page_ != nullptr) source_page_->SetDragSelectEnabled(drag_select_);
    ApplyFilter();
  });
}

void LibraryWindow::SavePrefs() {
  mira_gui::FrontendPrefs prefs;
  prefs.window_width = width();
  prefs.window_height = height();
  prefs.tile_width = tile_width_;
  prefs.source_tile_widths = source_tile_widths_;
  prefs.library_filter = CurrentFilterKey().toStdString();
  prefs.sort_by = sort_key_;
  prefs.sort_descending = sort_descending_;
  prefs.scan_on_startup = scan_on_startup_;
  const QList<int> sizes = splitter_->sizes();
  if (sizes.size() == 2) prefs.sidebar_width = sizes[0];
  // Blocking, not fire-and-forget: the async form's detached thread might
  // not reach the socket before the process exits on the last window's
  // close. Failure isn't reported: the cost is a remembered layout, not data.
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
  downloads_button_->setIcon(mira_gui::icons::For(Glyph::Download));
  shortcuts_button_->setIcon(mira_gui::icons::For(Glyph::Keyboard));
  about_button_->setIcon(mira_gui::icons::For(Glyph::Info));
  top_bar_divider_->setStyleSheet(
      QString("background: %1;").arg(mira_gui::theme::Current().border.name()));
  minimize_button_->setIcon(mira_gui::icons::For(Glyph::Minimize));
  maximize_button_->setIcon(
      mira_gui::icons::For(isMaximized() ? Glyph::Restore : Glyph::Maximize));
  close_button_->setIcon(mira_gui::icons::For(Glyph::Close));
  add_games_->setIcon(mira_gui::icons::For(Glyph::Plus, mira_gui::theme::Current().on_accent));
  runners_nav_->setIcon(mira_gui::icons::For(Glyph::Wrench));
  fetch_art_button_->setIcon(mira_gui::icons::For(Glyph::Image));
  manage_sources_button_->setIcon(mira_gui::icons::For(Glyph::Sliders, mira_gui::theme::Current().text_muted));
  grid_view_button_->setIcon(mira_gui::icons::For(Glyph::Grid));
  table_view_button_->setIcon(mira_gui::icons::For(Glyph::Table));

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
  constexpr const char* kSourceMime = "application/x-mira-source";
  if (auto* nav = qobject_cast<QPushButton*>(watched); nav != nullptr && source_navs_.contains(nav)) {
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
      source_drag_row_ = nav;
      source_drag_start_ = mouse->position().toPoint();
    } else if (event->type() == QEvent::MouseMove && source_drag_row_ == nav &&
               (mouse->buttons() & Qt::LeftButton) &&
               (mouse->position().toPoint() - source_drag_start_).manhattanLength() >=
                   QApplication::startDragDistance()) {
      const qsizetype index = source_navs_.indexOf(nav);
      auto* mime = new QMimeData();
      mime->setData(kSourceMime, mira_gui::AllSources()[index].id.toUtf8());
      auto* drag = new QDrag(nav);
      drag->setMimeData(mime);
      drag->setPixmap(nav->grab());
      drag->setHotSpot(source_drag_start_);
      source_drag_row_ = nullptr;
      nav->setDown(false);
      drag->exec(Qt::MoveAction);
      return true;
    }
  }
  if (watched == source_nav_container_) {
    const auto* drop = static_cast<QDropEvent*>(event);
    switch (event->type()) {
      case QEvent::DragEnter:
      case QEvent::DragMove: {
        if (!drop->mimeData()->hasFormat(kSourceMime)) return false;
        event->accept();
        const int row = SourceDropRow(drop->position().toPoint().y());
        QWidget* anchor = nullptr;
        for (QPushButton* nav : source_navs_) {
          if (nav->isVisible() && source_nav_layout_->indexOf(nav) == row) anchor = nav;
        }
        int y = 0;
        if (anchor != nullptr) {
          y = anchor->geometry().top() - 2;
        } else {
          for (QPushButton* nav : source_navs_) {
            if (nav->isVisible()) y = std::max(y, nav->geometry().bottom());
          }
        }
        source_drop_line_->setGeometry(0, y, source_nav_container_->width(), 2);
        source_drop_line_->show();
        source_drop_line_->raise();
        return true;
      }
      case QEvent::DragLeave:
        source_drop_line_->hide();
        return true;
      case QEvent::Drop: {
        source_drop_line_->hide();
        if (!drop->mimeData()->hasFormat(kSourceMime)) return false;
        const QString id = QString::fromUtf8(drop->mimeData()->data(kSourceMime));
        MoveSource(id, SourceDropRow(drop->position().toPoint().y()));
        event->accept();
        return true;
      }
      default:
        break;
    }
  }
  // The top bar's and sidebar header's own background, plus labels on them
  // (a label passes its clicks up); a click on a control goes to it instead.
  if (watched == top_bar_ || watched == sidebar_header_) {
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
  // A recently played row shows its game's hover card, after a tile's dwell.
  if (watched->property("hover_game").isValid()) {
    if (event->type() == QEvent::Enter) {
      if (recent_hover_ == nullptr) {
        recent_hover_ = new QTimer(this);
        recent_hover_->setSingleShot(true);
        recent_hover_->setInterval(mira_gui::card::kDwellMs);
        connect(recent_hover_, &QTimer::timeout, this, [this] {
          if (recent_hover_row_ == nullptr) return;
          const mira_gui::GameSummary* game =
              FindGame(recent_hover_row_->property("hover_game").toString().toStdString());
          if (game == nullptr) return;
          const QString hint = running_ids_.contains(game->id) ? "Right-click to stop it."
                               : game->status == "ready"       ? "Click to play."
                                                               : QString();
          ShowHoverCardFor(*game, QRect(recent_hover_row_->mapToGlobal(QPoint(0, 0)), recent_hover_row_->size()),
                           hint);
        });
      }
      recent_hover_row_ = qobject_cast<QWidget*>(watched);
      recent_hover_->start();
    } else if (event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) {
      if (recent_hover_ != nullptr) recent_hover_->stop();
      ShowHoverCard(nullptr);
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void LibraryWindow::closeEvent(QCloseEvent* event) {
  // Only for the window Attach() made the tray's; a secondary window
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
        // Neither Save() finishes synchronously, so quit for real only once it
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
  mira_gui::MiradClient::ClearArtThumbsBlocking();
  QMainWindow::closeEvent(event);
}

void LibraryWindow::OpenRunners() {
  if (runners_page_ != nullptr) return;
  if (ClassicShown()) CloseClassicView();
  if (source_page_ != nullptr) CloseSource();
  runners_page_ = new mira_gui::RunnersPage(downloads_, this);
  runners_page_->SetGames(games_);
  main_stack_->addWidget(runners_page_);
  main_stack_->setCurrentWidget(runners_page_);
  SetSourceControlsEnabled(false);
  UpdateLibraryNavActive();
}

void LibraryWindow::CloseRunners() {
  if (runners_page_ == nullptr) return;
  main_stack_->setCurrentWidget(grid_page_);
  main_stack_->removeWidget(runners_page_);
  runners_page_->deleteLater();
  runners_page_ = nullptr;
  SetSourceControlsEnabled(true);
  UpdateLibraryNavActive();
}

void LibraryWindow::OpenAbout() {
  QDialog dialog(this);
  dialog.setWindowTitle("About Mira");
  auto* layout = new QVBoxLayout(&dialog);
  layout->addWidget(new mira_gui::AboutPanel(&dialog));
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.resize(560, dialog.sizeHint().height());
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
      mira_gui::notify::FailedRequest(this, "Could not scan the library.", result.error);
      return;
    }
    // New games show up in the grid on their own; only "nothing happened"
    // has no visible result of its own.
    if (result.added == 0 && result.missing == 0 && result.restored == 0) {
      mira_gui::notify::Notice(this, "Scan finished. No changes.");
    }
    RefreshGames();
  });
}

void LibraryWindow::ImportSteamLibrary() {
  mira_gui::MiradClient::ScanSteamAsync(this, [this](mira_gui::SteamScanResult result) {
    if (!result.ok) {
      mira_gui::notify::FailedRequest(this, "Could not import from Steam.", result.error);
      return;
    }
    if (result.added == 0) mira_gui::notify::Notice(this, "No new Steam games found.");
    RefreshGames();
  });
}

void LibraryWindow::ImportLutrisLibrary() {
  mira_gui::MiradClient::ImportLutrisAsync(this, [this](mira_gui::LutrisImportResult result) {
    if (!result.ok) {
      mira_gui::notify::FailedRequest(this, "Could not import from Lutris.", result.error);
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
      mira_gui::notify::FailedRequest(this, "Could not regenerate desktop entries.", result.error);
      return;
    }
    mira_gui::notify::Notice(this, "Desktop entries regenerated.");
  });
}

void LibraryWindow::RemoveAllDesktopEntries() {
  // desktop_entries.enabled is the only lever that actually makes Sync()
  // remove every mira-<id>.desktop entry rather than immediately rewriting
  // them (see desktop::DesktopEntries::Sync), so there's no "wipe once, stay
  // enabled" concept, so this is honest about turning the setting off too.
  if (!mira_gui::notify::Confirm(
          this, "Remove All Desktop Entries",
          "This turns off desktop entries and deletes every one Mira generated. "
          "Re-enable them any time in Settings → Desktop Entries.",
          "Remove all", /*destructive=*/true)) {
    return;
  }
  const mira_gui::ConfigEdit edit{"desktop_entries.enabled", "a boolean", "false"};
  mira_gui::MiradClient::PatchConfigAsync(
      this, {edit}, [this](mira_gui::PatchConfigResult patch_result) {
        if (!patch_result.ok) {
          mira_gui::notify::FailedRequest(this, "Could not turn off desktop entries.", patch_result.error);
          return;
        }
        mira_gui::MiradClient::SyncDesktopEntriesAsync(
            this, [this](mira_gui::DesktopEntrySyncResult sync_result) {
              if (!sync_result.ok) {
                mira_gui::notify::FailedRequest(this, "Could not remove the desktop entries.",
                                                sync_result.error);
                return;
              }
              mira_gui::notify::Notice(this, "Desktop entries removed.");
            });
      });
}

QWidget* LibraryWindow::BuildTopBar() {
  top_bar_ = new QWidget(this);
  top_bar_->setObjectName("top_bar");
  // Catches a press/double-click on the bar's own empty background; see
  // eventFilter. A click on any child widget never reaches here.
  top_bar_->installEventFilter(this);

  auto* layout = new QHBoxLayout(top_bar_);
  layout->setContentsMargins(8, 4, 6, 4);
  layout->setSpacing(8);

  layout->addStretch(1);

  view_toggle_ = new QWidget(top_bar_);
  view_toggle_->setObjectName("view_toggle");
  auto* toggle_layout = new QHBoxLayout(view_toggle_);
  toggle_layout->setContentsMargins(0, 0, 0, 0);
  toggle_layout->setSpacing(0);
  grid_view_button_ = new QToolButton(view_toggle_);
  grid_view_button_->setToolTip("Grid view");
  table_view_button_ = new QToolButton(view_toggle_);
  table_view_button_->setToolTip("Table view");
  for (QToolButton* button : {grid_view_button_, table_view_button_}) {
    button->setCheckable(true);
    button->setAutoRaise(true);
    toggle_layout->addWidget(button);
  }
  connect(grid_view_button_, &QToolButton::clicked, this, &LibraryWindow::ShowLibrary);
  // A second click on Table goes back to the grid.
  connect(table_view_button_, &QToolButton::clicked, this, [this] {
    if (ClassicShown()) {
      CloseClassicView();
    } else {
      OpenClassicView();
    }
  });
  layout->addWidget(view_toggle_);

  zoom_ = new QSlider(Qt::Horizontal, top_bar_);
  zoom_->setRange(120, 260);
  zoom_->setValue(tile_width_);
  zoom_->setMaximumWidth(120);
  zoom_->setToolTip("Tile size");
  connect(zoom_, &QSlider::valueChanged, this, &LibraryWindow::Zoom);
  layout->addWidget(zoom_);

  downloads_button_ = new QToolButton(top_bar_);
  downloads_button_->setAutoRaise(true);
  downloads_button_->setToolTip("Downloads");
  // Its count's text is taller than the icon; the bar shouldn't grow for it.
  downloads_button_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
  connect(downloads_button_, &QToolButton::clicked, this,
          [this] { downloads_panel_->ShowBelow(downloads_button_); });
  layout->addWidget(downloads_button_);

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
    if (library_tabs_ != nullptr) library_tabs_->SetCurrent(CurrentFilterKey());
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
  connect(sort_direction_, &QToolButton::clicked, this, [this] {
    sort_descending_ = !sort_descending_;
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
  // The popover's own button shows the current direction too.
  sort_direction_->setArrowType(sort_descending_ ? Qt::DownArrow : Qt::UpArrow);
  sort_direction_->setToolTip(sort_descending_ ? "Descending. Click for ascending."
                                               : "Ascending. Click for descending.");
}

QWidget* LibraryWindow::BuildSidebar() {
  auto* sidebar = new QWidget(this);
  sidebar->setObjectName("left_sidebar");
  // Rows with their own menu handle it first; the rest fall through to here.
  sidebar->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(sidebar, &QWidget::customContextMenuRequested, this,
          [this, sidebar](const QPoint& pos) { ShowSidebarMenu(sidebar->mapToGlobal(pos)); });
  auto* layout = new QVBoxLayout(sidebar);
  layout->setContentsMargins(10, 14, 10, 10);
  layout->setSpacing(2);

  auto* header = new QWidget(sidebar);
  auto* header_layout = new QHBoxLayout(header);
  header_layout->setContentsMargins(6, 0, 6, 10);
  header_layout->setSpacing(8);
  auto* badge = new QLabel("M", header);
  badge->setObjectName("sidebar_badge");
  // Like the top bar, the header moves the window.
  sidebar_header_ = header;
  header->installEventFilter(this);
  badge->setFixedSize(22, 22);
  badge->setAlignment(Qt::AlignCenter);
  header_layout->addWidget(badge);
  auto* title = new QLabel("Mira", header);
  title->setObjectName("sidebar_title");
  header_layout->addWidget(title, /*stretch=*/1);
  layout->addWidget(header);

  // Always visible (not just a "back" affordance): checked/highlighted
  // exactly when the grid is the current content; see UpdateLibraryNavActive.
  library_nav_ = new QPushButton("Library", sidebar);
  library_nav_->setObjectName("library_nav");
  library_nav_->setFlat(true);
  library_nav_->setCheckable(true);
  library_nav_->setChecked(true);
  connect(library_nav_, &QPushButton::clicked, this, &LibraryWindow::ShowLibrary);
  layout->addWidget(library_nav_);

  runners_nav_ = new QPushButton("Runners", sidebar);
  runners_nav_->setFlat(true);
  runners_nav_->setCheckable(true);
  connect(runners_nav_, &QPushButton::clicked, this, &LibraryWindow::OpenRunners);
  layout->addWidget(runners_nav_);

  settings_button_ = new QPushButton("Settings", sidebar);
  settings_button_->setObjectName("sidebar_settings");
  settings_button_->setFlat(true);
  connect(settings_button_, &QPushButton::clicked, this, [this] { OpenSettings(); });
  layout->addWidget(settings_button_);

  // The PINNED, SOURCES and RECENTLY PLAYED rows scroll, so they never set
  // the window's minimum height.
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

  pinned_heading_ = SidebarHeading(nav_content, "PINNED");
  pinned_heading_->setVisible(false);
  nav_layout->addWidget(pinned_heading_);
  pinned_layout_ = new QVBoxLayout();
  pinned_layout_->setSpacing(2);
  nav_layout->addLayout(pinned_layout_);

  auto* sources_heading = new QWidget(nav_content);
  auto* sources_heading_layout = new QHBoxLayout(sources_heading);
  sources_heading_layout->setContentsMargins(0, 0, 0, 0);
  sources_heading_layout->addWidget(SidebarHeading(sources_heading, "SOURCES"), /*stretch=*/1);
  manage_sources_button_ = new QToolButton(sources_heading);
  manage_sources_button_->setAutoRaise(true);
  manage_sources_button_->setToolTip("Manage sources");
  connect(manage_sources_button_, &QToolButton::clicked, this, &LibraryWindow::OpenManageSources);
  sources_heading_layout->addWidget(manage_sources_button_, 0, Qt::AlignBottom);
  nav_layout->addWidget(sources_heading);

  source_nav_layout_ = new QVBoxLayout();
  source_nav_layout_->setSpacing(4);
  // Rows drag to reorder; see eventFilter.
  source_nav_container_ = nav_content;
  nav_content->setAcceptDrops(true);
  nav_content->installEventFilter(this);
  source_drop_line_ = new QWidget(nav_content);
  source_drop_line_->setFixedHeight(2);
  source_drop_line_->setStyleSheet(QString("background: %1;").arg(mira_gui::theme::Current().accent.name()));
  source_drop_line_->hide();
  for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
    auto* nav = new QPushButton(source.name, nav_content);
    nav->setFlat(true);
    nav->setCheckable(true);
    nav->setVisible(false);  // until UpdateSourceNavs knows it's set up
    nav->setObjectName("source_nav");
    nav->setIconSize(QSize(22, 22));
    source_counts_.append(AddTrailingLabel(nav));
    connect(nav, &QPushButton::clicked, this, [this, source] { OpenSource(source); });
    nav->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(nav, &QWidget::customContextMenuRequested, this,
            [this, nav, source](const QPoint& pos) { ShowSourceMenu(source, nav->mapToGlobal(pos)); });
    nav->installEventFilter(this);
    source_nav_layout_->addWidget(nav);
    source_navs_.append(nav);
  }
  nav_layout->addLayout(source_nav_layout_);

  recent_heading_ = SidebarHeading(nav_content, "RECENTLY PLAYED");
  recent_heading_->setVisible(false);
  nav_layout->addWidget(recent_heading_);
  recent_layout_ = new QVBoxLayout();
  recent_layout_->setSpacing(2);
  nav_layout->addLayout(recent_layout_);

  nav_layout->addStretch(1);
  nav_scroll->setWidget(nav_content);
  layout->addWidget(nav_scroll, /*stretch=*/1);

  auto* actions = new QHBoxLayout();
  actions->setSpacing(6);
  add_games_ = new QToolButton(sidebar);
  add_games_->setObjectName("add_games");
  add_games_->setText("Add games");
  add_games_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  add_games_->setPopupMode(QToolButton::InstantPopup);
  // QToolButton stays content-sized otherwise.
  add_games_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* add_games_menu = new QMenu(add_games_);
  add_games_menu->addAction("Scan library folders", this, &LibraryWindow::ScanLibrary);
  add_games_menu->addAction("Import Steam library", this, &LibraryWindow::ImportSteamLibrary);
  add_games_menu
      ->addAction("Import Lutris games", this, &LibraryWindow::ImportLutrisLibrary)
      ->setToolTip(
          "Add the Wine games from Lutris's database. Nothing is moved or renamed, so the games "
          "stay playable in Lutris too.");
  add_games_menu
      ->addAction("Import desktop entries…", this, &LibraryWindow::ImportDesktopEntries)
      ->setToolTip(
          "Pick installed apps from your application menu to add as games. This includes "
          "Flatpak apps.");
  add_games_menu->addSeparator();
  add_games_menu->addAction("Add game manually…", this, &LibraryWindow::AddGameManually);
  add_games_->setMenu(add_games_menu);
  actions->addWidget(add_games_, /*stretch=*/1);

  fetch_art_button_ = new QToolButton(sidebar);
  fetch_art_button_->setObjectName("fetch_art");
  // mirad only fetches on its own for a newly found game, so one that failed
  // once stays bare until asked again.
  fetch_art_button_->setToolTip("Fetch missing cover art for every game without one");
  connect(fetch_art_button_, &QToolButton::clicked, this, &LibraryWindow::FetchMissingArtwork);
  actions->addWidget(fetch_art_button_);
  layout->addSpacing(6);
  layout->addLayout(actions);

  // Replaces the old bottom bar entirely.
  footer_ = new QLabel(sidebar);
  footer_->setProperty("role", "muted");
  footer_->setContentsMargins(6, 8, 6, 2);
  footer_->setWordWrap(true);
  layout->addWidget(footer_);

  return sidebar;
}

// The same tab row a source page has, holding the grid's filter, sort and
// search, then the Continue playing cards.
QWidget* LibraryWindow::BuildLibraryHeader() {
  auto* top = new QWidget(this);
  auto* layout = new QVBoxLayout(top);
  layout->setContentsMargins(11, 14, 11, 0);
  layout->setSpacing(14);

  library_tabs_ = new mira_gui::TabRow(top);
  for (const auto& [key, label] : kFilterTabs) library_tabs_->AddTab(key, label);
  library_tabs_->SetAlert("attention", true);
  library_tabs_->SetCurrent("all");
  connect(library_tabs_, &mira_gui::TabRow::CurrentChanged, this,
          [this](const QString& key) { filters_->setCurrentRow(FilterRow(key)); });

  // Pill summarizing filter+sort, opening a self-dismissing Qt::Popup with
  // the actual rows/buttons.
  filter_sort_popover_ = BuildFilterSortPopover();
  auto* pill = new FilterSortButton(top);
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
    filter_sort_popover_->setFixedWidth(qMax(240, filter_sort_button_->width()));
    const QPoint below_left = filter_sort_button_->mapToGlobal(QPoint(0, filter_sort_button_->height() + 4));
    filter_sort_popover_->move(below_left);
    filter_sort_popover_->show();
  };
  library_tabs_->SetTrailing(pill);
  search_ = new QLineEdit(top);
  search_->setObjectName("library_search");
  search_->setPlaceholderText("Search…");
  search_->setClearButtonEnabled(true);
  search_->setFixedWidth(240);
  connect(search_, &QLineEdit::textChanged, this, [this] { ApplyFilter(); });
  library_tabs_->SetTrailing(search_);
  layout->addWidget(library_tabs_);

  continue_row_ = new mira_gui::ContinueRow(artwork_, top);
  continue_row_->setVisible(false);
  connect(continue_row_, &mira_gui::ContinueRow::PlayToggled, this,
          [this](const QString& id) { RowClicked(id.toStdString()); });
  connect(continue_row_, &mira_gui::ContinueRow::MenuRequested, this,
          [this](const QString& id, const QPoint& pos) { ShowGameMenu(id.toStdString(), pos); });
  layout->addWidget(continue_row_);
  return top;
}

QWidget* LibraryWindow::BuildGrid() {
  auto* container = new QWidget(this);
  grid_layout_ = new QVBoxLayout(container);
  QVBoxLayout* layout = grid_layout_;
  // Plus the grid's own padding and tile inset, lines up with the header.
  layout->setContentsMargins(11, 0, 11, 0);
  layout->setSpacing(6);
  layout->addWidget(BuildLibraryHeader());

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
  grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
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
    // A game that still needs installing installs instead.
    if (status == "needs_install" && InstallText(id).isEmpty()) {
      const mira_gui::GameSummary* game = FindGame(id);
      if (game != nullptr) {
        mira_gui::actions::Install(this, id, game->install_path, QString::fromStdString(game->name));
      }
      return;
    }
    // Same rule as the context menu's Play entry and the Enter shortcut: a
    // game that isn't ready has nothing to launch, and /launch would just
    // 409. Stop needs no such guard: running_ids_ already reflects reality.
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

void LibraryWindow::Zoom(int width) {
  if (source_page_ == nullptr || tile_size_synced_) SetTileWidth(width);
  if (source_page_ == nullptr) return;
  if (!tile_size_synced_) source_tile_widths_[source_page_->property("source_id").toString().toStdString()] = width;
  source_page_->SetTileWidth(width);
}

int LibraryWindow::SourceTileWidth(const QString& id) const {
  constexpr int kDefaultSourceTileWidth = 150;  // smaller: a source page holds two grids
  if (tile_size_synced_) return tile_width_;
  const auto it = source_tile_widths_.find(id.toStdString());
  return it == source_tile_widths_.end() ? kDefaultSourceTileWidth : it->second;
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
  if (continue_row_->Shows(id.toStdString())) RefreshContinue();
  // One item, not a rebuild: artwork arrives one game at a time, and
  // ApplyFilter would drop the selection and scroll position on each.
  for (int row = 0; row < grid_->count(); ++row) {
    QListWidgetItem* item = grid_->item(row);
    if (item->data(mira_gui::GameTileDelegate::IdRole).toString() != id) continue;
    const mira_gui::GameSummary* game = FindGame(id.toStdString());
    if (game == nullptr) return;
    item->setData(Qt::DecorationRole, CoverFor(*game));
    // The edit page draws the same game at a different size, so it needs the
    // same nudge, since it has no way to notice the store changed under it. A
    // no-op if it isn't currently showing this game (or isn't open at all).
    if (game_edit_form_ != nullptr) game_edit_form_->RefreshCover();
    if (game_edit_backdrop_ != nullptr) game_edit_backdrop_->RefreshCover(id.toStdString());
    return;
  }
}

void LibraryWindow::InstallErrorNavigator() {
  const auto find_source = [](const std::string& id) -> const mira_gui::SourceInfo* {
    const auto& sources = mira_gui::AllSources();
    const auto it = std::ranges::find(sources, QString::fromStdString(id), &mira_gui::SourceInfo::id);
    return it != sources.end() ? &*it : nullptr;
  };
  QPointer<LibraryWindow> self(this);
  mira_gui::error_help::Navigator nav;
  nav.open_setting = [self](const QString& key) {
    if (self) self->OpenSettings(key);
  };
  nav.open_runners = [self] {
    if (self) self->OpenRunners();
  };
  nav.open_source = [self, find_source](const std::string& id) {
    if (const mira_gui::SourceInfo* source = find_source(id); self && source != nullptr) self->OpenSource(*source);
  };
  nav.open_game_settings = [self](const std::string& id) {
    if (self) self->OpenGameDialog(id);
  };
  nav.view_log = [self](const std::string& id) {
    if (!self) return;
    const mira_gui::GameSummary* game = self->FindGame(id);
    mira_gui::actions::ViewLog(self, id, game != nullptr ? QString::fromStdString(game->name) : QString());
  };
  nav.start_daemon = [self] {
    if (!self) return;
    // Kept, not deleted after Ready: its destructor stops a mirad it started.
    if (self->daemon_supervisor_ == nullptr) {
      self->daemon_supervisor_ = new mira_gui::DaemonSupervisor(self);
      connect(self->daemon_supervisor_, &mira_gui::DaemonSupervisor::Ready, self,
              [self] { self->RefreshHealth(/*force_scan=*/false); });
      connect(self->daemon_supervisor_, &mira_gui::DaemonSupervisor::Failed, self,
              [self](const QString& error) {
                mira_gui::notify::FailedWithHint(self, "Could not start mirad.", error,
                                                 "Mira looks for mirad next to itself, then on PATH.");
              });
    }
    self->daemon_supervisor_->EnsureRunning();
  };
  nav.source_name = [find_source](const std::string& id) {
    const mira_gui::SourceInfo* source = find_source(id);
    return source != nullptr ? source->name : QString();
  };
  mira_gui::error_help::SetNavigator(std::move(nav));
}

void LibraryWindow::ShowSteamGridDbNotice(bool asked_for) {
  // Only when the user actually asked for art: a background fetch after a
  // scan hitting this would otherwise nag on every launch. Once per session,
  // however many games report it.
  if (!asked_for || steamgriddb_notice_shown_) return;
  steamgriddb_notice_shown_ = true;

  mira_gui::notify::FailedWithAction(
      this, "No SteamGridDB API key set.",
      "Non-Steam games need a free SteamGridDB API key before Mira can find cover art for "
      "them, because there is no other free source. Steam games are unaffected.",
      QString(), "Open the Metadata settings…", [this] { OpenSettings("steamgriddb.api_key"); });
}

void LibraryWindow::FetchMissingArtwork() {
  // One request for the whole library; mirad decides what's missing.
  mira_gui::MiradClient::RefreshMissingArtworkAsync(
      this, [this](mira_gui::RefreshMissingArtworkResult result) {
        if (!result.ok) {
          mira_gui::notify::FailedRequest(this, "Could not fetch missing cover art.", result.error);
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
            mira_gui::notify::FailedRequest(this, "Could not refresh metadata.", result.error);
          }
          return;
        }
        // 202: fetch runs on the daemon, reports back as an event. Remembered
        // so ShowSteamGridDbNotice knows this game was asked about.
        awaiting_metadata_.insert(id);
      });
}

void LibraryWindow::RefreshHealth(bool force_scan) {
  mira_gui::MiradClient::CheckHealthAsync(this, [this, force_scan](mira_gui::HealthStatus status) {
    mirad_reachable_ = status.reachable;
    if (status.reachable) {
      RescanAndRefreshGames(force_scan);
    } else {
      games_.clear();
      ApplyFilter();
      mira_gui::notify::FailedRequest(this, "Mira lost its connection to mirad.",
                                      mira_gui::ApiError(status.detail, mira_gui::ApiError::kUnreachable));
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
      mira_gui::notify::FailedRequest(this, "Could not list games.", visible.error);
      games_.clear();
      ApplyFilter();
      return;
    }
    mira_gui::MiradClient::ListGamesAsync(
        this,
        [this, visible = std::move(visible)](mira_gui::GamesResult hidden) mutable {
          games_ = std::move(visible.games);
          // A failed second fetch just means the Hidden filter shows
          // nothing this round, and not worth failing the whole refresh over.
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
  // Store launchers (Battle.net, ...) live on their source pages, not here.
  if (game.source == "launcher") return false;
  if (key == "hidden") return HasTag(game, "hidden");
  // Every other filter excludes a hidden game: "not displayed by default"
  // means not in "All games" either, not just off the initial screen.
  if (HasTag(game, "hidden")) return false;
  if (key == "all") return true;
  if (key == "running") return running_ids_.contains(game.id);
  if (key == "never") return !game.last_played_at.has_value();
  if (key == "attention") {
    return game.status == "needs_install" || game.status == "broken" || game.status == "missing";
  }
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
    library_tabs_->SetCount(key, count);
  }
}

void LibraryWindow::ApplyFilter() {
  const std::string previously_selected = selected_id_;
  UpdateFilterCounts();

  // Sorted here, not at fetch time, so a sort change costs a tile rebuild,
  // not a round trip.
  mira_gui::SortGames(games_, sort_key_, sort_descending_);

  std::vector<const mira_gui::GameSummary*> visible;
  for (const mira_gui::GameSummary& game : games_) {
    if (MatchesFilter(game)) visible.push_back(&game);
  }
  const int shown = static_cast<int>(visible.size());

  bool same_tiles = grid_->count() == shown;
  for (int i = 0; same_tiles && i < shown; ++i) {
    same_tiles = grid_->item(i)->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString() ==
                 visible[i]->id;
  }

  if (same_tiles) {
    // Same games in the same order (most game.updated events): refresh the
    // tiles in place, keeping selection, scroll and hover.
    for (int i = 0; i < shown; ++i) FillTile(grid_->item(i), *visible[i]);
  } else {
    QSet<QString> selected;
    for (const QListWidgetItem* item : grid_->selectedItems()) {
      selected.insert(item->data(mira_gui::GameTileDelegate::IdRole).toString());
    }
    // grid_->clear() deletes every item; a stale last_hover_item_/pending
    // dwell timer pointing at one of them would be a use-after-free the next
    // time it fires.
    grid_->ForgetItems();
    ShowHoverCard(nullptr);

    grid_->blockSignals(true);
    grid_->clear();
    QListWidgetItem* to_select = nullptr;
    for (const mira_gui::GameSummary* game : visible) {
      auto* item = new QListWidgetItem(grid_);
      item->setData(mira_gui::GameTileDelegate::IdRole, QString::fromStdString(game->id));
      FillTile(item, *game);
      if (game->id == previously_selected) to_select = item;
      if (selected.contains(QString::fromStdString(game->id))) item->setSelected(true);
    }
    grid_->blockSignals(false);

    if (to_select != nullptr) {
      grid_->setCurrentItem(to_select, QItemSelectionModel::NoUpdate);
      to_select->setSelected(true);
    } else if (!previously_selected.empty()) {
      // Selected game was filtered away or removed, so don't keep showing it.
      selected_id_.clear();
    }
  }

  empty_hint_->setVisible(shown == 0);
  if (shown == 0) {
    empty_hint_->setText(games_.empty() ? "No games in the library yet."
                                        : "No games match this filter.");
  }

  // Two lines: count first (what you're looking at), connection status
  // second (background fact, muted further by the dot standing in for a word).
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();
  footer_->setText(QString("%1 of %2 games shown<br><span style='color:%3'>●</span> %4 %5")
                       .arg(shown)
                       .arg(games_.size())
                       .arg((mirad_reachable_ ? tokens.success : tokens.error).name(),
                            mirad_reachable_ ? QString("Connected via") : QString("Not connected to"),
                            QString::fromStdString(mira_gui::MiradClient::ResolveSocketPath())));

  RefreshClassicTable();
  if (source_page_ != nullptr) source_page_->SetGames(games_, running_ids_);
  if (runners_page_ != nullptr) runners_page_->SetGames(games_);
  UpdateSourceNavs();
  RefreshSidebarGames();
  RefreshContinue();
}

void LibraryWindow::ScheduleApplyFilter() {
  if (filter_timer_ == nullptr) {
    filter_timer_ = new QTimer(this);
    filter_timer_->setSingleShot(true);
    connect(filter_timer_, &QTimer::timeout, this, [this] { ApplyFilter(); });
  }
  if (!filter_timer_->isActive()) filter_timer_->start(0);
}

void LibraryWindow::FillTile(QListWidgetItem* item, const mira_gui::GameSummary& game) {
  item->setData(mira_gui::GameTileDelegate::NameRole, QString::fromStdString(game.name));
  item->setData(mira_gui::GameTileDelegate::StatusRole, QString::fromStdString(game.status));
  item->setData(mira_gui::GameTileDelegate::RunningRole, running_ids_.contains(game.id));
  item->setData(mira_gui::GameTileDelegate::PinnedRole, HasTag(game, kPinnedTag));
  item->setData(mira_gui::GameTileDelegate::SourceRole, QString::fromStdString(game.source));
  item->setData(mira_gui::GameTileDelegate::StatusTextRole, InstallText(game.id));
  item->setData(Qt::DecorationRole, CoverFor(game));
}

const mira_gui::GameSummary* LibraryWindow::FindGame(const std::string& id) const {
  for (const mira_gui::GameSummary& game : games_) {
    if (game.id == id) return &game;
  }
  return nullptr;
}

void LibraryWindow::UpsertGame(const mira_gui::GameSummary& game) { UpsertGames({game}); }

void LibraryWindow::UpsertGames(const std::vector<mira_gui::GameSummary>& games) {
  for (const mira_gui::GameSummary& game : games) {
    // A rename changes the placeholder's initials, so the rendered tile is
    // stale even though the fetched artwork behind it isn't.
    artwork_->InvalidateRendering(game.id);
    const auto existing = std::ranges::find(games_, game.id, &mira_gui::GameSummary::id);
    if (existing != games_.end()) {
      *existing = game;
    } else {
      games_.push_back(game);
    }
  }
  ScheduleApplyFilter();
}

void LibraryWindow::RemoveGame(const std::string& id) { RemoveGames({id}); }

void LibraryWindow::RemoveGames(const std::vector<std::string>& ids) {
  std::erase_if(games_, [&](const mira_gui::GameSummary& game) { return std::ranges::contains(ids, game.id); });
  if (std::ranges::contains(ids, selected_id_)) selected_id_.clear();
  ScheduleApplyFilter();
}

void LibraryWindow::SelectionChanged() {
  // Not the grid on screen (Settings or classic table instead), and a stray
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

  const QRect tile = grid_->visualItemRect(item);
  ShowHoverCardFor(*game, QRect(grid_->viewport()->mapToGlobal(tile.topLeft()), tile.size()));
}

void LibraryWindow::ShowHoverCardFor(const mira_gui::GameSummary& game, const QRect& anchor,
                                     const QString& hint) {
  if (hover_card_ == nullptr) hover_card_ = new mira_gui::HoverCard(this);
  hover_card_->ShowGame(game, running_ids_.contains(game.id), hint);
  hover_card_->PopUpBeside(anchor);
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
  // NoUpdate: the default ClearAndSelect would drop a drag or Ctrl+A selection
  // whenever the right-clicked tile isn't already the current one.
  grid_->setCurrentItem(item, QItemSelectionModel::NoUpdate);

  if (grid_->selectedItems().size() > 1) {
    std::vector<std::string> ids;
    for (QListWidgetItem* selected : grid_->selectedItems()) {
      ids.push_back(selected->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString());
    }
    ShowBatchMenu(ids, grid_->viewport()->mapToGlobal(pos));
    return;
  }

  ShowGameMenu(item->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString(),
               grid_->viewport()->mapToGlobal(pos));
}

void LibraryWindow::ShowGameMenu(const std::string& id, const QPoint& global_pos,
                                 const std::function<void(QMenu&)>& extra) {
  const mira_gui::GameSummary* current_game = FindGame(id);
  if (current_game == nullptr) return;
  const QString name = QString::fromStdString(current_game->name);
  const std::string status = current_game->status;
  const bool running = running_ids_.contains(id);

  QMenu menu(this);
  QAction* play = menu.addAction(running ? "Stop" : "Play");
  play->setEnabled(running || status == "ready");
  if (extra) extra(menu);
  QAction* details = menu.addAction("Game settings…");
  QAction* folder = menu.addAction("Open install folder");
  QAction* more_details = menu.addAction("More details…");
  const bool pinned = current_game != nullptr && HasTag(*current_game, kPinnedTag);
  QAction* toggle_pinned = menu.addAction(pinned ? "Unpin" : "Pin to sidebar");
  menu.addSeparator();
  // Both halves of the needs_install escape hatch: run the installer inside
  // this game's prefix, then say it worked. "Run in prefix" is offered for
  // every game; only needs_install can be "marked installed".
  const bool installing = !InstallText(id).isEmpty();
  QAction* install = menu.addAction(installing ? "Installing…" : "Install…");
  install->setEnabled((status == "needs_install" || status == "broken") && !installing);
  install->setToolTip("Run this game's installer, or pick a different one");
  QAction* run_in_prefix = menu.addAction("Run in prefix…");
  QAction* finish_install = menu.addAction("Mark as installed");
  finish_install->setEnabled(status == "needs_install");
  finish_install->setToolTip(status == "needs_install"
                                 ? "Mark this game as ready once its executable points at the "
                                   "installed program"
                                 : "Only available for a game that still needs installing");
  QAction* refresh_metadata = menu.addAction("Refresh metadata && cover art");
  QAction* view_log = menu.addAction("View log…");
  QAction* winetricks = menu.addAction("Run winetricks…");
  QAction* relocate = menu.addAction("Move to Mira's folders…");
  relocate->setToolTip("Move this game's files and prefix into the library and prefix folders");
  const bool native = current_game != nullptr && current_game->platform == "native";
  winetricks->setEnabled(!native);
  winetricks->setToolTip(native ? "Native games have no Wine or Proton prefix." : QString());
  // Resolved (not on GameSummary), and the menu item's own label is the only
  // place that state shows, so it's fetched synchronously here rather than
  // asking first and acting second: a local socket round trip, once, before
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
  menu.addSeparator();
  const bool hidden = current_game != nullptr && HasTag(*current_game, "hidden");
  QAction* toggle_hidden = menu.addAction(hidden ? "Unhide" : "Hide");
  toggle_hidden->setToolTip(hidden
                                ? "Show this game in the library again"
                                : "Keep this game out of the library until you ask for it "
                                  "(Ctrl+H, or the Hidden filter)");
  menu.addSeparator();
  QAction* remove = menu.addAction("Remove from library…");

  QAction* chosen = menu.exec(global_pos);
  if (chosen == play) {
    ToggleRunning(id);
  } else if (chosen == details) {
    OpenGameDialog(id);
  } else if (chosen == folder) {
    mira_gui::actions::OpenInstallFolder(
        this, current_game != nullptr ? current_game->install_path : std::string());
  } else if (chosen == more_details) {
    OpenGameDetailPage(id);
  } else if (chosen == install) {
    mira_gui::actions::Install(
        this, id, current_game != nullptr ? current_game->install_path : std::string(), name);
  } else if (chosen == relocate) {
    mira_gui::actions::Relocate(this, {{id, name}}, [this] { RefreshGames(); });
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
  } else if (chosen == toggle_pinned) {
    ToggleTag(id, kPinnedTag);
  } else if (chosen == toggle_hidden) {
    ToggleTag(id, "hidden");
  } else if (chosen == remove) {
    mira_gui::actions::Delete(this, id, name, [this] { RefreshGames(); });
  }
}

void LibraryWindow::ShowBatchMenu(const std::vector<std::string>& ids, const QPoint& global_pos) {
  const int count = static_cast<int>(ids.size());

  int pinned = 0;
  int hidden = 0;
  for (const std::string& id : ids) {
    const mira_gui::GameSummary* game = FindGame(id);
    if (game != nullptr && HasTag(*game, kPinnedTag)) ++pinned;
    if (game != nullptr && HasTag(*game, "hidden")) ++hidden;
  }

  // Each offered for the games it would change, so a mixed selection gets both.
  QMenu menu(this);
  QAction* refresh_metadata = menu.addAction(QString("Refresh metadata && cover art (%1)").arg(count));
  QAction* pin = pinned < count ? menu.addAction(QString("Pin to sidebar (%1)").arg(count - pinned)) : nullptr;
  QAction* unpin = pinned > 0 ? menu.addAction(QString("Unpin (%1)").arg(pinned)) : nullptr;
  QAction* hide = hidden < count ? menu.addAction(QString("Hide (%1)").arg(count - hidden)) : nullptr;
  if (hide != nullptr) {
    hide->setToolTip("Keep these games out of the library until you ask for them (Ctrl+H, or the Hidden filter)");
  }
  QAction* unhide = hidden > 0 ? menu.addAction(QString("Unhide (%1)").arg(hidden)) : nullptr;
  auto* desktop_menu = menu.addMenu("Desktop entry");
  QAction* add_desktop_entry = desktop_menu->addAction("Add to application menu");
  QAction* remove_desktop_entry = desktop_menu->addAction("Remove from application menu");
  QAction* relocate = menu.addAction(QString("Move to Mira's folders… (%1)").arg(count));
  menu.addSeparator();
  QAction* remove = menu.addAction(QString("Remove from library… (%1)").arg(count));

  QAction* chosen = menu.exec(global_pos);

  std::vector<std::pair<std::string, QString>> named;
  named.reserve(count);
  for (const std::string& id : ids) {
    const mira_gui::GameSummary* game = FindGame(id);
    named.emplace_back(id, game != nullptr ? QString::fromStdString(game->name) : QString::fromStdString(id));
  }

  if (chosen == nullptr) return;  // dismissed; also keeps it from matching an action left out above
  if (chosen == refresh_metadata) {
    // No notice: covers visibly update as each fetch lands.
    for (const std::string& game_id : ids) RefreshMetadata(game_id, /*announce=*/false);
  } else if (chosen == pin || chosen == unpin) {
    BatchSetTag(ids, kPinnedTag, chosen == pin);
  } else if (chosen == hide || chosen == unhide) {
    BatchSetTag(ids, "hidden", chosen == hide);
  } else if (chosen == add_desktop_entry) {
    mira_gui::actions::BatchSetDesktopEntry(this, ids, /*enabled=*/true);
  } else if (chosen == remove_desktop_entry) {
    mira_gui::actions::BatchSetDesktopEntry(this, ids, /*enabled=*/false);
  } else if (chosen == relocate) {
    mira_gui::actions::Relocate(this, named, [this] { RefreshGames(); });
  } else if (chosen == remove) {
    mira_gui::actions::BatchDelete(this, named, [this] { RefreshGames(); });
  }
}

void LibraryWindow::BatchSetTag(const std::vector<std::string>& ids, const std::string& tag, bool present) {
  mira_gui::GamesPatch patch;
  for (const std::string& id : ids) {
    const mira_gui::GameSummary* game = FindGame(id);
    if (game != nullptr && HasTag(*game, tag) != present) patch.ids.push_back(id);
  }
  if (patch.ids.empty()) return;
  (present ? patch.add_tags : patch.remove_tags).push_back(tag);

  const bool one = patch.ids.size() == 1;
  mira_gui::MiradClient::PatchGamesAsync(this, patch, [this, tag, one](mira_gui::PatchGamesResult result) {
    if (!result.ok) {
      const QString games = one ? "this game's" : "these games'";
      mira_gui::notify::FailedRequest(this,
                                      tag == "hidden" ? QString("Could not change %1 visibility.").arg(games)
                                                      : QString("Could not change whether %1 pinned.")
                                                            .arg(one ? "this game is" : "these games are"),
                                      result.error);
      return;
    }
    // Applied from the reply rather than waiting for games.updated, so the
    // change feels instant. No toast: the games visibly moving is the feedback.
    UpsertGames(result.games);
  });
}

void LibraryWindow::ToggleTag(const std::string& id, const std::string& tag) {
  const mira_gui::GameSummary* game = FindGame(id);
  if (game != nullptr) BatchSetTag({id}, tag, !HasTag(*game, tag));
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
    game_edit_backdrop_ = nullptr;
    game_edit_cover_ = nullptr;
    game_edit_stack_ = nullptr;
    game_edit_picker_ = nullptr;
    game_edit_hero_button_ = nullptr;
    game_edit_cover_button_ = nullptr;
    game_edit_back_ = nullptr;
    game_edit_advanced_ = nullptr;
    game_edit_save_ = nullptr;
  }
  RefreshGames();
}

bool LibraryWindow::ArtPickerOpen() const {
  return game_edit_picker_ != nullptr && game_edit_stack_->currentWidget() == game_edit_picker_;
}

void LibraryWindow::OpenArtPicker(const std::string& slot) {
  if (game_edit_form_ == nullptr) return;
  if (ArtPickerOpen()) {
    // The button of the slot already open closes it again.
    if (game_edit_picker_->slot() == slot) return CloseArtPicker();
    game_edit_backdrop_->SetPreview(QString::fromStdString(game_edit_picker_->slot()), QPixmap());
    game_edit_cover_->SetPreview(QPixmap());
  }
  if (game_edit_picker_ == nullptr) {
    game_edit_picker_ = new mira_gui::ArtPickerPanel(game_edit_form_->id(), game_edit_stack_);
    game_edit_stack_->addWidget(game_edit_picker_);
    connect(game_edit_picker_, &mira_gui::ArtPickerPanel::Previewed, this,
            [this](const QString& preview_slot, const QPixmap& preview) {              game_edit_backdrop_->SetPreview(preview_slot, preview);
              if (preview_slot == "cover") game_edit_cover_->SetPreview(preview);
            });
    // The footer is the picker's only while it's open; an apply can land after.
    connect(game_edit_picker_, &mira_gui::ArtPickerPanel::PickChanged, this, [this](bool has_change) {
      if (ArtPickerOpen()) game_edit_save_->setEnabled(has_change);
    });
    connect(game_edit_picker_, &mira_gui::ArtPickerPanel::PickActivated, this, [this] {
      if (ArtPickerOpen()) game_edit_save_->click();
    });
    connect(game_edit_picker_, &mira_gui::ArtPickerPanel::ApplyFailed, this,
            [this](const QString& failed_slot, const QString& error) {              if (game_edit_backdrop_ != nullptr) game_edit_backdrop_->SetPreview(failed_slot, QPixmap());
              if (game_edit_cover_ != nullptr && failed_slot == "cover") game_edit_cover_->SetPreview(QPixmap());
              mira_gui::notify::Failed(this, "Could not change the art.", error);
            });
  }
  const bool hero = slot == "hero";
  game_edit_hero_button_->setChecked(hero);
  game_edit_cover_button_->setChecked(!hero);
  game_edit_back_->setText("Cancel");
  game_edit_advanced_->hide();
  game_edit_save_->setText(hero ? "Use this hero" : "Use this cover");
  game_edit_save_->setEnabled(false);
  game_edit_stack_->setCurrentWidget(game_edit_picker_);
  game_edit_picker_->Open(slot);
}

void LibraryWindow::CloseArtPicker(bool applied) {
  if (!ArtPickerOpen()) return;
  // An applied pick stays on screen until its art arrives in its place.
  if (!applied) {
    game_edit_backdrop_->SetPreview(QString::fromStdString(game_edit_picker_->slot()), QPixmap());
    game_edit_cover_->SetPreview(QPixmap());
  }
  game_edit_hero_button_->setChecked(false);
  game_edit_cover_button_->setChecked(false);
  game_edit_back_->setText("← Back");
  game_edit_advanced_->show();
  game_edit_save_->setText("Save");
  game_edit_save_->setEnabled(true);
  game_edit_stack_->setCurrentIndex(0);
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
  // These act on a hidden grid. library_nav_ stays clickable, since it's the way
  // back out. Disabling filter_sort_button_ alone blocks its popover too.
  for (QWidget* control :
       {filter_sort_button_, static_cast<QWidget*>(add_games_), static_cast<QWidget*>(search_),
        static_cast<QWidget*>(zoom_), static_cast<QWidget*>(settings_button_),
        static_cast<QWidget*>(view_toggle_), static_cast<QWidget*>(fetch_art_button_)}) {
    control->setEnabled(enabled);
  }
  // Back from Settings onto a source page: the grid is still covered.
  if (enabled && (source_page_ != nullptr || runners_page_ != nullptr)) SetSourceControlsEnabled(false);
  if (enabled && ClassicShown()) zoom_->setEnabled(false);
}

void LibraryWindow::UpdateLibraryNavActive() {
  using mira_gui::icons::Glyph;
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();

  const bool classic_active = ClassicShown();
  const bool library_active = (GridShown() || classic_active) && !GameEditOpen();
  if (library_nav_ != nullptr) {
    library_nav_->setChecked(library_active);
    library_nav_->setIcon(
        mira_gui::icons::For(Glyph::Home, library_active ? tokens.on_accent : tokens.text));
  }
  if (runners_nav_ != nullptr) {
    const bool runners_active = runners_page_ != nullptr && content_stack_->currentWidget() == splitter_;
    runners_nav_->setChecked(runners_active);
    runners_nav_->setIcon(mira_gui::icons::For(Glyph::Wrench, runners_active ? tokens.on_accent : tokens.text));
  }
  if (grid_view_button_ != nullptr) {
    grid_view_button_->setChecked(!classic_active);
    table_view_button_->setChecked(classic_active);
  }
  const QString open_source = content_stack_->currentWidget() == splitter_ && source_page_ != nullptr
                                  ? source_page_->property("source_id").toString()
                                  : QString();
  const std::vector<mira_gui::SourceInfo>& sources = mira_gui::AllSources();
  for (int i = 0; i < source_navs_.size() && i < static_cast<int>(sources.size()); ++i) {
    const bool active = sources[i].id == open_source;
    source_navs_[i]->setChecked(active);
    source_navs_[i]->setIcon(SourceIcon(sources[i], active));
    source_counts_[i]->setStyleSheet(active ? QString("color: %1;").arg(tokens.on_accent.name()) : QString());
  }
}

QIcon LibraryWindow::SourceIcon(const mira_gui::SourceInfo& source, bool active) const {
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();
  if (!source_icons_) return mira_gui::icons::For(mira_gui::icons::Glyph::Dot, active ? tokens.on_accent : source.color);
  // The source's initial on its color, like its page's header.
  const qreal ratio = devicePixelRatioF();
  constexpr int kSize = 22;
  QPixmap pixmap(QSize(kSize, kSize) * ratio);
  pixmap.setDevicePixelRatio(ratio);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(source.color);
  painter.drawRoundedRect(QRectF(0, 0, kSize, kSize), 6, 6);
  QFont font = this->font();
  font.setPixelSize(12);
  font.setWeight(QFont::Bold);
  painter.setFont(font);
  painter.setPen(Qt::white);
  painter.drawText(QRectF(0, 0, kSize, kSize), Qt::AlignCenter, source.name.left(1));
  return QIcon(pixmap);
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
            // The screen closing back to the grid is already the feedback:
            // a save the user just triggered isn't the background-result
            // case a toast is for.
            CloseSettings();
            LoadPrefs();
            RefreshSourceNavs();
          });
  layout->addWidget(settings_panel_, /*stretch=*/1);

  settings_panel_->AddSectionAction(
      "Library", "Move Games into Mira's Folders",
      "Moves each game's files into the library folder and its prefix into the prefix folder. "
      "Changing those folders does not move anything until you run this.",
      "Move games…", [this] { RelocateLibrary(); });
  settings_panel_->AddSectionAction(
      "Desktop Entries", "Regenerate Desktop Entries",
      "Rewrites Mira's desktop entries now, so changes to the desktop entry settings apply "
      "without waiting for the next library change.",
      "Regenerate", [this] { SyncDesktopEntries(); });
  settings_panel_->AddSectionAction("Desktop Entries", "Remove All Desktop Entries",
                                    "Turns off desktop entries and deletes every one Mira generated.",
                                    "Remove…", [this] { RemoveAllDesktopEntries(); });

  // Pinned under the settings nav's category list.
  auto* actions = new QWidget();
  auto* actions_layout = new QHBoxLayout(actions);
  actions_layout->setContentsMargins(0, 0, 0, 0);
  actions_layout->setSpacing(6);
  auto* back = new QPushButton("← Back", actions);
  connect(back, &QPushButton::clicked, this, &LibraryWindow::RequestCloseSettings);
  auto* reset = new QPushButton("Reset", actions);
  reset->setToolTip("Discard unsaved changes and go back to the last saved settings.");
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
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();
  const mira_gui::GameSummary* game = FindGame(id);

  // The hero fills the card's top and fades into it; everything below sits
  // over it.
  game_edit_backdrop_ = new mira_gui::HeroBackdrop(artwork_);
  QWidget* card = game_edit_backdrop_;
  if (game != nullptr) game_edit_backdrop_->ShowGame(*game);
  // ~70% of the window, not a hardcoded constant -- recomputed per open
  // since the window can resize between edits.
  card->setFixedSize(qRound(width() * 0.7), qRound(height() * 0.7));

  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto* actions = new QHBoxLayout();
  actions->setSpacing(8);
  auto hero_action = [card](const QString& text) {
    auto* button = new QPushButton(text, card);
    button->setObjectName("hero_action");
    return button;
  };
  QPushButton* choose_hero = hero_action("Change hero");
  choose_hero->setCheckable(true);
  // setChecked is OpenArtPicker/CloseArtPicker's, not the click's.
  connect(choose_hero, &QPushButton::clicked, this, [this, choose_hero] {
    choose_hero->setChecked(!choose_hero->isChecked());
    OpenArtPicker("hero");
  });
  QPushButton* choose_cover = hero_action("Change cover");
  choose_cover->setCheckable(true);
  connect(choose_cover, &QPushButton::clicked, this, [this, choose_cover] {
    choose_cover->setChecked(!choose_cover->isChecked());
    OpenArtPicker("cover");
  });
  game_edit_hero_button_ = choose_hero;
  game_edit_cover_button_ = choose_cover;
  QPushButton* close = hero_action(QString());
  close->setIcon(mira_gui::icons::For(mira_gui::icons::Glyph::Close));
  close->setToolTip("Close");
  connect(close, &QPushButton::clicked, this, &LibraryWindow::RequestCloseGameEdit);
  for (QPushButton* button : {choose_hero, choose_cover, close}) actions->addWidget(button);

  // Name and status over the art, level with the buttons. Static: the form
  // below has its own Name field. The shadow, in the surface's color, keeps
  // it off the art's detail.
  auto* header = new QHBoxLayout();
  header->setContentsMargins(24, 16, 14, 16);
  header->setSpacing(16);
  auto* identity = new QVBoxLayout();
  identity->setSpacing(4);
  auto* title = new QLabel(game != nullptr ? QString::fromStdString(game->name) : "Game settings", card);
  title->setObjectName("game_edit_title");
  title->setWordWrap(true);
  auto* shadow = new QGraphicsDropShadowEffect(title);
  shadow->setColor(tokens.surface);
  shadow->setBlurRadius(18);
  shadow->setOffset(0, 1);
  title->setGraphicsEffect(shadow);
  identity->addWidget(title);
  if (game != nullptr) {
    const bool running = running_ids_.contains(game->id);
    const QColor status_color = mira_gui::StatusColor(running ? "running" : game->status);
    QString source = mira_gui::StatusLabel(game->source);
    for (const mira_gui::SourceInfo& info : mira_gui::AllSources()) {
      if (info.id.toStdString() == game->source) source = info.name;
    }
    QStringList facts;
    if (!source.isEmpty()) facts << source;
    if (!game->platform.empty()) facts << mira_gui::StatusLabel(game->platform);
    if (game->play_seconds > 0) facts << mira_gui::FormatPlaytime(game->play_seconds) + " played";
    auto* status = new QLabel(
        QString("<span style='color:%1; font-weight:600;'>%2</span>&nbsp;&nbsp;%3")
            .arg(status_color.name(), running ? "Playing" : mira_gui::StatusLabel(game->status),
                 facts.join(" · ").toHtmlEscaped()),
        card);
    status->setTextFormat(Qt::RichText);
    identity->addWidget(status);
  }
  // The cover, which the hero otherwise hides, and where a picked one previews.
  game_edit_cover_ = new mira_gui::CoverChip(artwork_, card);
  if (game != nullptr) game_edit_cover_->ShowGame(*game);
  header->addWidget(game_edit_cover_, 0, Qt::AlignTop);
  header->addLayout(identity, /*stretch=*/1);
  header->addLayout(actions);
  header->setAlignment(actions, Qt::AlignTop);
  layout->addLayout(header);

  // Translucent, so the art still shows through at its top edge.
  auto* panel = new QWidget(card);
  panel->setObjectName("game_edit_panel");
  panel->setAttribute(Qt::WA_StyledBackground);
  QColor panel_color = tokens.window;
  panel_color.setAlphaF(0.82);
  panel->setStyleSheet(QString("QWidget#game_edit_panel { background: rgba(%1, %2, %3, %4); border: 1px solid "
                               "%5; border-radius: %6px; }")
                           .arg(panel_color.red())
                           .arg(panel_color.green())
                           .arg(panel_color.blue())
                           .arg(panel_color.alpha())
                           .arg(tokens.border.name())
                           .arg(tokens.radius_panel));
  auto* panel_layout = new QVBoxLayout(panel);
  panel_layout->setContentsMargins(0, 0, 0, 0);
  auto* panel_row = new QHBoxLayout();
  panel_row->setContentsMargins(20, 0, 20, 0);
  panel_row->addWidget(panel);
  layout->addLayout(panel_row, /*stretch=*/1);

  game_edit_stack_ = new QStackedWidget(panel);
  panel_layout->addWidget(game_edit_stack_);
  auto* scroll = new QScrollArea(game_edit_stack_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");
  scroll->viewport()->setAutoFillBackground(false);
  game_edit_stack_->addWidget(scroll);
  auto* form_container = new QWidget();
  auto* form_container_layout = new QVBoxLayout(form_container);
  form_container_layout->setContentsMargins(18, 18, 18, 18);
  game_edit_form_ = new mira_gui::GameEditForm(id, form_container);
  game_edit_form_->SetArtworkStore(artwork_);
  game_edit_form_->SetArtColumnVisible(false);
  form_container_layout->addWidget(game_edit_form_);
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
            // The overlay closing back to the grid is already the feedback:
            // a save the user just triggered isn't the background-result
            // case a toast is for.
            CloseGameEdit();
          });
  scroll->setWidget(form_container);

  auto* footer = new QWidget(card);
  auto* footer_layout = new QHBoxLayout(footer);
  footer_layout->setContentsMargins(20, 12, 20, 16);
  // While the art picker is open these are its Cancel and Use.
  auto* back = new QPushButton("← Back", footer);
  connect(back, &QPushButton::clicked, this, [this] {
    if (ArtPickerOpen()) {
      CloseArtPicker();
    } else {
      RequestCloseGameEdit();
    }
  });
  auto* save = new QPushButton("Save", footer);
  save->setDefault(true);
  connect(save, &QPushButton::clicked, this, [this] {
    if (!ArtPickerOpen()) return game_edit_form_->Save();
    game_edit_picker_->Apply();
    CloseArtPicker(/*applied=*/true);
  });
  // In the footer rather than at the bottom of the scrolling form.
  game_edit_form_->SetAdvancedButtonVisible(false);
  auto* advanced = new QPushButton("Advanced settings…", footer);
  advanced->setToolTip("Per-game overrides of the global settings.");
  connect(advanced, &QPushButton::clicked, game_edit_form_, &mira_gui::GameEditForm::OpenAdvanced);
  game_edit_back_ = back;
  game_edit_advanced_ = advanced;
  game_edit_save_ = save;
  footer_layout->addWidget(back);
  footer_layout->addStretch(1);
  footer_layout->addWidget(advanced);
  footer_layout->addWidget(save);
  layout->addWidget(footer);

  return card;
}

void LibraryWindow::OpenSource(const mira_gui::SourceInfo& source) {
  if (ClassicShown()) CloseClassicView();
  CloseRunners();
  if (source_page_ != nullptr) {
    main_stack_->removeWidget(source_page_);
    source_page_->deleteLater();
  }
  source_page_ = new mira_gui::SourcePage(source, artwork_, downloads_, source_page_tabs_,
                                          SourceTileWidth(source.id), this);
  source_page_->SetGames(games_, running_ids_);
  source_page_->SetDragSelectEnabled(drag_select_);
  source_page_->setProperty("source_id", source.id);
  connect(source_page_, &mira_gui::SourcePage::LibraryChanged, this, [this, id = source.id] {
    NoteImported(id);
    RefreshGames();
  });
  connect(source_page_, &mira_gui::SourcePage::OpenSettingsRequested, this,
          [this](const QString& key) { OpenSettings(key); });
  connect(source_page_, &mira_gui::SourcePage::Removed, this, [this, id = source.id] {
    CloseSource();
    ForgetSource(id);
  });
  // Same rule as the grid's double-click: only a ready game has anything to launch.
  connect(source_page_, &mira_gui::SourcePage::GameMenuRequested, this,
          [this](const QString& id, const QPoint& pos, const QString& update_ref) {
            mira_gui::SourcePage* page = source_page_;
            ShowGameMenu(id.toStdString(), pos, [page, update_ref](QMenu& menu) {
              if (update_ref.isEmpty() || page == nullptr) return;
              QObject::connect(menu.addAction("Update"), &QAction::triggered, page,
                               [page, update_ref] { page->UpdateTitle(update_ref); });
            });
          });
  connect(source_page_, &mira_gui::SourcePage::BatchMenuRequested, this,
          [this](const QStringList& ids, const QPoint& pos) {
            std::vector<std::string> games;
            for (const QString& id : ids) games.push_back(id.toStdString());
            ShowBatchMenu(games, pos);
          });
  connect(source_page_, &mira_gui::SourcePage::PlayRequested, this, [this](const QString& id) {
    const mira_gui::GameSummary* game = FindGame(id.toStdString());
    if (game == nullptr) return;
    if (running_ids_.contains(game->id) || game->status == "ready") ToggleRunning(game->id);
  });
  main_stack_->addWidget(source_page_);
  main_stack_->setCurrentWidget(source_page_);
  SetSourceControlsEnabled(false);
  const QSignalBlocker blocker(zoom_);
  zoom_->setValue(SourceTileWidth(source.id));
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
  const QSignalBlocker blocker(zoom_);
  zoom_->setValue(tile_width_);
  UpdateLibraryNavActive();
  RefreshSourceNavs();  // a sign-in or launcher install there changes the order
}

// The grid's search and filter leave with its page; the slider stays for a
// source page, which has tiles of its own to size.
void LibraryWindow::SetSourceControlsEnabled(bool enabled) {
  if (!enabled) ShowHoverCard(nullptr);
  zoom_->setEnabled(enabled || (source_page_ != nullptr && main_stack_->currentWidget() == source_page_));
}

bool LibraryWindow::GridShown() const {
  return content_stack_->currentWidget() == splitter_ && main_stack_->currentWidget() == grid_page_;
}

bool LibraryWindow::ClassicShown() const {
  return content_stack_->currentWidget() == splitter_ && main_stack_->currentWidget() == classic_page_;
}

QString LibraryWindow::InstallText(const std::string& id) const {
  using State = mira_gui::DownloadTracker::State;
  const mira_gui::DownloadTracker::Entry* entry =
      downloads_->Find(mira_gui::DownloadTracker::KeyFor(mira_gui::DownloadTracker::Kind::Game, QString(),
                                                         QString::fromStdString(id)));
  if (entry == nullptr || entry->state != State::Running) return QString();
  if (entry->bytes <= 0) return "Installing…";
  return "Installing… " + QLocale().formattedDataSize(entry->bytes);
}

void LibraryWindow::DownloadChanged(const QString& key) {
  const int running = downloads_->RunningCount();
  downloads_button_->setText(running > 0 ? QString::number(running) : QString());
  downloads_button_->setToolButtonStyle(running > 0 ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
  downloads_button_->setToolTip(running == 0 ? QString("Downloads")
                                             : QString("Downloads: %1 running").arg(running));

  // One tile's text, not a rebuild.
  if (!key.startsWith("game:")) return;
  const QString id = key.mid(5);
  for (int row = 0; row < grid_->count(); ++row) {
    QListWidgetItem* item = grid_->item(row);
    if (item->data(mira_gui::GameTileDelegate::IdRole).toString() == id) {
      item->setData(mira_gui::GameTileDelegate::StatusTextRole, InstallText(id.toStdString()));
    }
  }
}

void LibraryWindow::ShowGame(const std::string& id) {
  if (SettingsOpen()) RequestCloseSettings();
  if (GameEditOpen()) RequestCloseGameEdit();
  if (ClassicShown()) CloseClassicView();
  if (source_page_ != nullptr) CloseSource();
  CloseRunners();
  for (int row = 0; row < grid_->count(); ++row) {
    QListWidgetItem* item = grid_->item(row);
    if (item->isHidden() || item->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString() != id) {
      continue;
    }
    grid_->clearSelection();
    grid_->setCurrentItem(item);
    grid_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    return;
  }
  OpenGameDialog(id);
}

void LibraryWindow::RelocateLibrary() {
  if (!mira_gui::notify::Confirm(
          this, "Move Games into Mira's Folders",
          "Move every game's files into your games folder, and each prefix into the prefixes "
          "folder, named after the game? Games installed by a store (Steam, Epic, GOG, itch.io) "
          "keep their install folder; only the prefix moves. Games on another drive are copied "
          "then deleted, which can take a while.",
          "Move games")) {
    return;
  }
  mira_gui::notify::Notice(this, "Moving games into Mira's folders…");
  mira_gui::MiradClient::RelocateLibraryAsync(this, [this](mira_gui::RelocateLibraryResult result) {
    if (!result.ok) {
      mira_gui::notify::FailedRequest(this, "Could not move the games.", result.error);
      return;
    }
    if (result.failed > 0) {
      mira_gui::notify::FailedWithHint(
          this, QString("Could not move %1 game%2.").arg(result.failed).arg(result.failed == 1 ? "" : "s"),
          QString("%1 moved.").arg(result.moved), "mirad's log says why for each one.");
    } else {
      mira_gui::notify::Notice(this, result.moved == 0 ? QString("Every game was already in place.")
                                                       : QString("Moved %1 game%2.")
                                                             .arg(result.moved)
                                                             .arg(result.moved == 1 ? "" : "s"));
    }
    RefreshGames();
  });
}

void LibraryWindow::RefreshSourceNavs() {
  mira_gui::MiradClient::GetConfigAsync(this, [this](mira_gui::ConfigResult result) {
    if (!result.ok) return;
    disabled_sources_.clear();
    for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
      const auto found = result.values.find(source.id.toStdString() + ".enabled");
      if (found != result.values.end() && found->second == "false") disabled_sources_.insert(source.id);
    }
    UpdateSourceNavs();
  });
  // A store counts as set up once signed in, a launcher once installed.
  for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
    if (source.kind != mira_gui::SourceInfo::Kind::Store) continue;
    const QString id = source.id;
    mira_gui::MiradClient::GetStoreStatusAsync(
        this, id.toStdString(), [this, id](mira_gui::StoreStatusResult status) {
          source_ready_[id] = status.ok && status.authenticated;
          source_account_[id] = QString::fromStdString(status.account);
          UpdateSourceNavs();
        });
  }
  mira_gui::MiradClient::GetLaunchersAsync(this, [this](mira_gui::LaunchersResult result) {
    for (const mira_gui::LauncherInfo& launcher : result.launchers) {
      source_ready_[QString::fromStdString(launcher.id)] = launcher.installed;
    }
    UpdateSourceNavs();
  });
}

void LibraryWindow::UpdateSourceNavs() {
  if (source_nav_layout_ == nullptr) return;
  std::map<std::string, int> counts;
  for (const mira_gui::GameSummary& game : games_) ++counts[game.source];

  const std::vector<mira_gui::SourceInfo>& sources = mira_gui::AllSources();
  for (int i = 0; i < source_navs_.size() && i < static_cast<int>(sources.size()); ++i) {
    const QString& id = sources[i].id;
    const auto count = counts.find(id.toStdString());
    const int games = count == counts.end() ? 0 : count->second;
    const bool ready = games > 0 || source_ready_.value(id, false);
    source_navs_[i]->setVisible(ready && !hidden_sources_.contains(id) && !disabled_sources_.contains(id));
    // A store with games whose account is signed out wants a look.
    const bool signed_out = sources[i].kind == mira_gui::SourceInfo::Kind::Store && games > 0 &&
                            source_ready_.contains(id) && !source_ready_.value(id);
    QString label = show_source_counts_ ? QString::number(games) : QString();
    if (signed_out) label = mira_gui::StatusDot(mira_gui::theme::Current().warning) + label;
    source_counts_[i]->setText(label);
    source_navs_[i]->setToolTip(signed_out ? QString("Signed out of %1").arg(sources[i].name) : QString());
  }
  int row = 0;
  for (const QString& id : SourceOrder()) {
    for (int i = 0; i < static_cast<int>(sources.size()); ++i) {
      if (sources[i].id != id) continue;
      source_nav_layout_->removeWidget(source_navs_[i]);
      source_nav_layout_->insertWidget(row++, source_navs_[i]);
    }
  }
  UpdateLibraryNavActive();
}

std::vector<ManageSourcesDialog::Entry> LibraryWindow::SourceEntries() const {
  std::map<std::string, int> counts;
  for (const mira_gui::GameSummary& game : games_) ++counts[game.source];
  std::vector<ManageSourcesDialog::Entry> entries;
  for (const QString& id : SourceOrder()) {
    const auto source = std::ranges::find(mira_gui::AllSources(), id, &mira_gui::SourceInfo::id);
    const auto count = counts.find(id.toStdString());
    const int games = count == counts.end() ? 0 : count->second;
    entries.push_back({.source = *source,
                       .ready = games > 0 || source_ready_.value(id, false),
                       .enabled = !disabled_sources_.contains(id),
                       .games = games,
                       .in_sidebar = !hidden_sources_.contains(id),
                       .account = source_account_.value(id).toStdString(),
                       .imported_at = source_imported_at_.value(id, 0)});
  }
  return entries;
}

void LibraryWindow::NoteImported(const QString& id) {
  source_imported_at_[id] = QDateTime::currentSecsSinceEpoch();
  mira_gui::FrontendPrefs prefs;
  std::map<std::string, std::int64_t> imported;
  for (auto it = source_imported_at_.cbegin(); it != source_imported_at_.cend(); ++it) {
    imported[it.key().toStdString()] = it.value();
  }
  prefs.source_imported_at = std::move(imported);
  mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
}

void LibraryWindow::MoveSourceBy(const QString& id, int delta) {
  // Among sources of the same kind, as the dialog groups them.
  const auto kind_of = [](const QString& source_id) {
    return std::ranges::find(mira_gui::AllSources(), source_id, &mira_gui::SourceInfo::id)->kind;
  };
  std::vector<QString> order = SourceOrder();
  const auto at = std::ranges::find(order, id);
  if (at == order.end()) return;
  auto neighbour = at;
  do {
    if (delta < 0 && neighbour == order.begin()) return;
    neighbour += delta < 0 ? -1 : 1;
    if (neighbour == order.end()) return;
  } while (kind_of(*neighbour) != kind_of(id));
  std::iter_swap(at, neighbour);
  source_order_ = order;
  UpdateSourceNavs();
  mira_gui::FrontendPrefs prefs;
  std::vector<std::string> ids;
  for (const QString& source : order) ids.push_back(source.toStdString());
  prefs.source_order = std::move(ids);
  mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
}

void LibraryWindow::OpenManageSources() {
  ManageSourcesDialog dialog(this);
  dialog.SetEntries(SourceEntries());
  const auto refresh = [this, &dialog] { dialog.SetEntries(SourceEntries()); };
  connect(&dialog, &ManageSourcesDialog::SidebarToggled, this,
          [this](const QString& id, bool shown) { SetSourceHidden(id, !shown); });
  connect(&dialog, &ManageSourcesDialog::EnabledToggled, this, [this, refresh](const QString& id, bool on) {
    if (on) {
      disabled_sources_.remove(id);
    } else {
      disabled_sources_.insert(id);
    }
    UpdateSourceNavs();
    refresh();
    const mira_gui::ConfigEdit edit{(id + ".enabled").toStdString(), "a boolean", on ? "true" : "false"};
    mira_gui::MiradClient::PatchConfigAsync(this, {edit}, [this](mira_gui::PatchConfigResult result) {
      if (!result.ok) mira_gui::notify::FailedRequest(this, "Could not change that source.", result.error);
      RefreshSourceNavs();
    });
  });
  connect(&dialog, &ManageSourcesDialog::MoveRequested, this, [this, refresh](const QString& id, int delta) {
    MoveSourceBy(id, delta);
    refresh();
  });
  connect(&dialog, &ManageSourcesDialog::Imported, this, [this](const QString& id) {
    NoteImported(id);
    RefreshGames();
  });
  connect(&dialog, &ManageSourcesDialog::Removed, this, [this, refresh](const QString& id) {
    ForgetSource(id);
    refresh();
  });
  QString open_id;
  connect(&dialog, &ManageSourcesDialog::OpenRequested, this, [&open_id](const QString& id) { open_id = id; });
  dialog.exec();
  if (open_id.isEmpty()) return;
  for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
    if (source.id == open_id) OpenSource(source);
  }
}

void LibraryWindow::ForgetSource(const QString& id) {
  disabled_sources_.insert(id);
  source_ready_[id] = false;
  std::erase_if(games_, [&id](const mira_gui::GameSummary& game) { return QString::fromStdString(game.source) == id; });
  UpdateSourceNavs();
  RefreshGames();
  RefreshSourceNavs();
}

std::vector<QString> LibraryWindow::SourceOrder() const {
  std::vector<QString> order;
  for (const QString& id : source_order_) {
    const bool known = std::ranges::any_of(mira_gui::AllSources(),
                                           [&id](const mira_gui::SourceInfo& source) { return source.id == id; });
    if (known && std::ranges::find(order, id) == order.end()) order.push_back(id);
  }
  for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
    if (std::ranges::find(order, source.id) == order.end()) order.push_back(source.id);
  }
  return order;
}

int LibraryWindow::SourceDropRow(int y) const {
  // The layout row of the first visible source whose middle is below `y`.
  int best = -1;
  int best_top = 0;
  for (QPushButton* nav : source_navs_) {
    if (!nav->isVisible() || y >= nav->geometry().center().y()) continue;
    if (best == -1 || nav->geometry().top() < best_top) {
      best = source_nav_layout_->indexOf(nav);
      best_top = nav->geometry().top();
    }
  }
  return best;
}

void LibraryWindow::MoveSource(const QString& id, int before) {
  std::vector<QString> order = SourceOrder();
  QString before_id;
  if (before >= 0) {
    if (auto* nav = qobject_cast<QPushButton*>(source_nav_layout_->itemAt(before)->widget())) {
      before_id = mira_gui::AllSources()[source_navs_.indexOf(nav)].id;
    }
  }
  if (before_id == id) return;
  std::erase(order, id);
  const auto at = before_id.isEmpty() ? order.end() : std::ranges::find(order, before_id);
  order.insert(at, id);
  source_order_ = order;
  UpdateSourceNavs();

  mira_gui::FrontendPrefs prefs;
  std::vector<std::string> ids;
  for (const QString& source : order) ids.push_back(source.toStdString());
  prefs.source_order = std::move(ids);
  mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
}

void LibraryWindow::SetSourceHidden(const QString& id, bool hidden) {
  if (hidden) {
    hidden_sources_.insert(id);
  } else {
    hidden_sources_.remove(id);
  }
  UpdateSourceNavs();
  mira_gui::FrontendPrefs prefs;
  std::vector<std::string> ids;
  for (const QString& hidden_id : hidden_sources_) ids.push_back(hidden_id.toStdString());
  std::ranges::sort(ids);
  prefs.hidden_sources = std::move(ids);
  mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
}

void LibraryWindow::RefreshSidebarGames() {
  if (recent_layout_ == nullptr) return;

  // Pinned games by name, matching the grid: hidden pins only under the Hidden filter.
  const bool showing_hidden = CurrentFilterKey() == "hidden";
  std::vector<const mira_gui::GameSummary*> pinned;
  for (const mira_gui::GameSummary& game : games_) {
    if (HasTag(game, kPinnedTag) && HasTag(game, "hidden") == showing_hidden) pinned.push_back(&game);
  }
  std::ranges::sort(pinned, [](const mira_gui::GameSummary* a, const mira_gui::GameSummary* b) {
    return QString::compare(QString::fromStdString(a->name), QString::fromStdString(b->name),
                            Qt::CaseInsensitive) < 0;
  });
  FillSidebarSection(pinned_heading_, pinned_layout_, pinned, pinned_signature_);

  // Every running game, then up to recent_count_ others by last played. A
  // hidden game shows only while it runs, so it can still be stopped.
  std::vector<const mira_gui::GameSummary*> running;
  std::vector<const mira_gui::GameSummary*> played;
  for (const mira_gui::GameSummary& game : games_) {
    if (running_ids_.contains(game.id)) {
      running.push_back(&game);
    } else if (game.last_played_at && !HasTag(game, "hidden")) {
      played.push_back(&game);
    }
  }
  std::ranges::sort(played, [](const mira_gui::GameSummary* a, const mira_gui::GameSummary* b) {
    return *a->last_played_at > *b->last_played_at;
  });
  if (played.size() > static_cast<size_t>(recent_count_)) played.resize(recent_count_);
  played.insert(played.begin(), running.begin(), running.end());
  FillSidebarSection(recent_heading_, recent_layout_, played, recent_signature_);
}

void LibraryWindow::FillSidebarSection(QLabel* heading, QVBoxLayout* layout,
                                       const std::vector<const mira_gui::GameSummary*>& games,
                                       QString& signature) {
  // Most refreshes (every game.updated) change nothing shown here; rebuilding
  // anyway makes the rows flicker.
  QString wanted = mira_gui::theme::Current().running.name();
  for (const mira_gui::GameSummary* game : games) {
    wanted += QString("\n%1\t%2\t%3\t%4")
                  .arg(QString::fromStdString(game->id), QString::fromStdString(game->name),
                       QString::fromStdString(game->status), running_ids_.contains(game->id) ? "1" : "0");
  }
  if (wanted == signature) return;
  signature = wanted;

  QWidget* parent = heading->parentWidget();
  parent->setUpdatesEnabled(false);
  // deleteLater: a row's own click or menu may be what got us here. Hidden
  // first: a popup's nested event loop would otherwise keep it painted.
  while (QLayoutItem* item = layout->takeAt(0)) {
    if (QWidget* row = item->widget()) {
      row->hide();
      row->deleteLater();
    }
    delete item;
  }
  heading->setVisible(!games.empty());
  for (const mira_gui::GameSummary* game : games) layout->addWidget(MakeSidebarGameRow(*game, parent));
  parent->setUpdatesEnabled(true);
}

QPushButton* LibraryWindow::MakeSidebarGameRow(const mira_gui::GameSummary& game, QWidget* parent) {
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();
  const bool is_running = running_ids_.contains(game.id);
  auto* row = new QPushButton(QString::fromStdString(game.name), parent);
  row->setFlat(true);
  row->setIcon(mira_gui::icons::For(mira_gui::icons::Glyph::Dot, is_running ? tokens.running : tokens.border));
  const std::string id = game.id;
  if (is_running) {
    AddTrailingLabel(row)->setText("Playing");
  } else if (game.status == "ready") {
    connect(row, &QPushButton::clicked, this, [this, id] { RowClicked(id); });
  }
  row->setProperty("hover_game", QString::fromStdString(id));
  row->installEventFilter(this);
  row->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(row, &QWidget::customContextMenuRequested, this,
          [this, row, id](const QPoint& pos) { ShowGameMenu(id, row->mapToGlobal(pos)); });
  return row;
}

void LibraryWindow::RowClicked(const std::string& id) {
  if (last_row_click_.isValid() && last_row_click_.elapsed() < QApplication::doubleClickInterval()) return;
  last_row_click_.start();
  const mira_gui::GameSummary* game = FindGame(id);
  if (game == nullptr) return;
  if (running_ids_.contains(id) || game->status == "ready") ToggleRunning(id);
}

void LibraryWindow::RefreshContinue() {
  if (continue_row_ == nullptr) return;
  // Only over the whole library: under a filter or a search it's noise.
  std::vector<const mira_gui::GameSummary*> games;
  if (continue_row_enabled_ && CurrentFilterKey() == "all" && search_->text().trimmed().isEmpty()) {
    for (const mira_gui::GameSummary& game : games_) {
      if (HasTag(game, "hidden") || game.source == "launcher") continue;
      if (running_ids_.contains(game.id) || game.last_played_at) games.push_back(&game);
    }
    std::ranges::sort(games, [this](const mira_gui::GameSummary* a, const mira_gui::GameSummary* b) {
      const bool a_running = running_ids_.contains(a->id);
      if (a_running != running_ids_.contains(b->id)) return a_running;
      return a->last_played_at.value_or(0) > b->last_played_at.value_or(0);
    });
    if (games.size() > static_cast<size_t>(continue_count_)) games.resize(continue_count_);
  }
  continue_row_->SetGames(games, running_ids_);
}

void LibraryWindow::ShowSourceMenu(const mira_gui::SourceInfo& source, const QPoint& global_pos) {
  QMenu menu(this);
  QAction* open = menu.addAction("Open " + source.name);
  // Neighbours among the visible rows, in their shown order.
  std::vector<QPushButton*> shown;
  for (int row = 0; row < source_nav_layout_->count(); ++row) {
    auto* nav = qobject_cast<QPushButton*>(source_nav_layout_->itemAt(row)->widget());
    if (nav != nullptr && nav->isVisible()) shown.push_back(nav);
  }
  QPushButton* self = source_navs_[std::ranges::find(mira_gui::AllSources(), source.id, &mira_gui::SourceInfo::id) -
                                   mira_gui::AllSources().begin()];
  const auto position = std::ranges::find(shown, self);
  QAction* up = menu.addAction("Move up");
  up->setEnabled(position != shown.end() && position != shown.begin());
  QAction* down = menu.addAction("Move down");
  down->setEnabled(position != shown.end() && position + 1 != shown.end());
  QAction* hide = menu.addAction("Hide from sidebar");
  menu.addSeparator();
  QAction* manage = menu.addAction("Manage sources…");
  QAction* settings = menu.addAction("Sidebar settings…");
  QAction* chosen = menu.exec(global_pos);
  if (chosen == open) {
    OpenSource(source);
  } else if (chosen == up) {
    MoveSource(source.id, source_nav_layout_->indexOf(*(position - 1)));
  } else if (chosen == down) {
    MoveSource(source.id, position + 2 == shown.end() ? -1 : source_nav_layout_->indexOf(*(position + 2)));
  } else if (chosen == hide) {
    SetSourceHidden(source.id, true);
  } else if (chosen == manage) {
    OpenManageSources();
  } else if (chosen == settings) {
    OpenSettings(mira_gui::SettingsPanel::kSidebarKey);
  }
}

void LibraryWindow::ShowSidebarMenu(const QPoint& global_pos) {
  QMenu menu(this);
  QAction* manage = menu.addAction("Manage sources…");
  QAction* settings = menu.addAction("Sidebar settings…");
  QAction* chosen = menu.exec(global_pos);
  if (chosen == manage) {
    OpenManageSources();
  } else if (chosen == settings) {
    OpenSettings(mira_gui::SettingsPanel::kSidebarKey);
  }
}

void LibraryWindow::ShowLibrary() {
  if (SettingsOpen()) {
    RequestCloseSettings();
  } else if (GameEditOpen()) {
    RequestCloseGameEdit();
  } else if (ClassicShown()) {
    CloseClassicView();
  } else if (source_page_ != nullptr) {
    CloseSource();
  } else if (runners_page_ != nullptr) {
    CloseRunners();
  }
  UpdateLibraryNavActive();
}

// Filter, sort and search apply to the table too; only tile size doesn't.
void LibraryWindow::OpenClassicView() {
  if (source_page_ != nullptr) CloseSource();
  CloseRunners();
  ShowHoverCard(nullptr);
  main_stack_->setCurrentWidget(classic_page_);
  zoom_->setEnabled(false);
  UpdateLibraryNavActive();
}

void LibraryWindow::CloseClassicView() {
  main_stack_->setCurrentWidget(grid_page_);
  zoom_->setEnabled(true);
  UpdateLibraryNavActive();
}

QWidget* LibraryWindow::BuildClassicPage() {
  auto* page = new QWidget(this);
  auto* layout = new QVBoxLayout(page);

  classic_table_ = new QTableWidget(0, 7, page);
  classic_table_->setHorizontalHeaderLabels(
      {"Name", "Status", "Platform", "Runner", "Last Played", "Playtime", ""});
  classic_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  classic_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  classic_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  classic_table_->setAlternatingRowColors(true);
  classic_table_->verticalHeader()->setVisible(false);
  classic_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int column = 1; column <= 5; ++column) {
    classic_table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
  }
  classic_table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
  // No column sort until one is clicked: rows keep the sidebar's sort order.
  classic_table_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
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

  // Same source and same filter as the grid: one games_ list, two
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
          QString("Can't launch: %1").arg(mira_gui::StatusLabel(game.status).toLower()));
    }
    connect(launch_button, &QPushButton::clicked, this, [this, id] { ToggleRunning(id); });
    actions_layout->addWidget(launch_button);

    auto* delete_button = new QPushButton("Delete", classic_table_);
    connect(delete_button, &QPushButton::clicked, this, [this, id, name] {
      mira_gui::actions::Delete(this, id, name, [this] { RefreshGames(); });
    });
    actions_layout->addWidget(delete_button);

    classic_table_->setCellWidget(row, 6, actions_widget);
    // ResizeToContents ignores cell widgets, which clipped the buttons. The
    // cell also loses base.qss's 4px item padding on each side.
    actions_widget->ensurePolished();
    classic_table_->setColumnWidth(
        6, std::max(classic_table_->columnWidth(6), actions_widget->sizeHint().width() + 8));
  }
  classic_table_->setSortingEnabled(true);
}

void LibraryWindow::HandleGameEvent(const std::string& type, const std::string& data) {
  // Before this, events are mirad's replayed history: apply them, announce nothing.
  if (type == "stream.live") {
    events_live_ = true;
    return;
  }

  if (type == "notification") {
    mira_gui::NotificationEvent event;
    if (events_live_ && mira_gui::MiradClient::ParseNotification(data, &event)) {
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

  // Doesn't consume it: the toasts below still want installs.
  downloads_->HandleEvent(type, data);

  if (mira_gui::StoreEvent art; mira_gui::MiradClient::ParseTitleArtworkEvent(type, data, &art)) {
    if (art.state == "ready") artwork_->TitleArtworkReady(art.source + "-" + art.ref);
    return;
  }

  if (mira_gui::InstallEvent install; mira_gui::MiradClient::ParseInstallEvent(type, data, &install)) {
    const mira_gui::GameSummary* game = FindGame(install.id);
    const QString name = game != nullptr ? QString::fromStdString(game->name) : QString("A game");
    if (!events_live_) {
      // History: the grid below still picks up the result.
    } else if (install.state == "failed") {
      mira_gui::notify::FailedRequest(this, "Could not install " + name + ".", install.error);
    } else if (install.state == "finished") {
      mira_gui::notify::Notice(this, name + " is installed.");
    }
    ScheduleApplyFilter();
    return;
  }

  if (type == "game.removed") {
    const std::string id = mira_gui::MiradClient::ParseRemovedId(data);
    if (!id.empty()) RemoveGame(id);
    return;
  }
  if (type == "games.removed") {
    const std::vector<std::string> ids = mira_gui::MiradClient::ParseRemovedIds(data);
    if (!ids.empty()) RemoveGames(ids);
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
    // A crash soon after launch is a game that failed to start. A later one is
    // left to the game's status: plenty of games exit non-zero on a normal quit.
    if (events_live_ && state.state == "crashed" && state.played_seconds < kFailedStartSeconds) {
      const mira_gui::GameSummary* crashed = FindGame(state.id);
      const QString name = crashed != nullptr ? QString::fromStdString(crashed->name) : QString("The game");
      const std::string id = state.id;
      mira_gui::notify::FailedWithAction(
          this, name + " closed right after starting.", QString::fromStdString(state.error),
          "Its log usually says why. A different runner in the game's settings often helps.", "View log",
          [this, id, name] { mira_gui::actions::ViewLog(this, id, name); });
    }
    return;
  }

  if (type == "game.metadata_ready" || type == "game.metadata_failed") {
    mira_gui::MetadataEvent event;
    if (!mira_gui::MiradClient::ParseMetadataEvent(data, &event)) return;
    if (type == "game.metadata_ready") {
      // Only the artwork is refetched here; the rest of the metadata isn't
      // part of the game record, so nothing else in the library view changes.
      artwork_->Invalidate(event.id);
      return;
    }
    // The one failure worth interrupting for: it's fixable and never
    // transient: no SteamGridDB key means every non-Steam game keeps its
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
      if (game_edit_backdrop_ != nullptr) game_edit_backdrop_->RefreshHero(event.id);
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
  // left over": mirad also publishes runners.download.* and tricks.* here.
  if (type == "games.updated") {
    std::vector<mira_gui::GameSummary> games;
    if (mira_gui::MiradClient::ParseGameSummaries(data, &games)) UpsertGames(games);
    return;
  }
  if (type != "game.added" && type != "game.updated") return;

  mira_gui::GameSummary game;
  if (mira_gui::MiradClient::ParseGameSummary(data, &game)) UpsertGame(game);

  // open_config_on_add: open a newly detected game's settings to check them.
  // Only for a lone arrival; a scan that finds several opens nothing rather
  // than stacking cards.
  if (type == "game.added" && mira_gui::MiradClient::ParseOpenConfig(data) && !game.id.empty()) {
    pending_added_.push_back(game.id);
    if (added_timer_ == nullptr) {
      added_timer_ = new QTimer(this);
      added_timer_->setSingleShot(true);
      added_timer_->setInterval(1500);
      connect(added_timer_, &QTimer::timeout, this, [this] {
        const std::vector<std::string> added = std::move(pending_added_);
        pending_added_.clear();
        if (added.size() == 1 && GridShown() && !GameEditOpen() && FindGame(added.front()) != nullptr) {
          OpenGameDialog(added.front());
        }
      });
    }
    added_timer_->start();
  }
}
