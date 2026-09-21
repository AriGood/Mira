#include "LibraryWindow.h"
#include <algorithm>

#include <QAbstractItemView>
#include <QAction>
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
#include <QCloseEvent>
#include <QComboBox>
#include <QEventLoop>
#include <QPushButton>
#include <QRubberBand>
#include <QScrollArea>
#include <QSet>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <iterator>
#include <optional>

#include "../client/MiradClient.h"
#include "../dialogs/AddManualGameDialog.h"
#include "../dialogs/ArtworkPickerDialog.h"
#include "../dialogs/DesktopEntryImportDialog.h"
#include "../dialogs/GameDetailDialog.h"
#include "../dialogs/GameDetailPageDialog.h"
#include "../dialogs/RunnerDialog.h"

#include "../ui/CoverArt.h"
#include "../ui/GameActions.h"
#include "../ui/GameDetailsPanel.h"
#include "../ui/GameEditForm.h"
#include "../ui/GameTileDelegate.h"
#include "../ui/Icons.h"
#include "../ui/KeyBindings.h"
#include "../ui/LibrarySort.h"
#include "../ui/Notify.h"
#include "../ui/SettingsPanel.h"
#include "../ui/Shortcuts.h"
#include "../ui/Theme.h"
#include "../ui/Tray.h"
#include "MainWindow.h"

// setViewportMargins is protected on QAbstractScrollArea; this just republishes
// it so ApplyLayoutTokens() can pad the tiles without also inseting the
// scrollbar (a container's own contents margins would do both).
//
// Also implements its own drag-to-select rather than relying on
// QAbstractItemView's built-in rubber band: that only starts when the press
// lands on genuinely empty viewport space, but every tile here fills its
// whole grid cell (setSpacing(0), the visual gap between tiles is the
// delegate's own padding within each cell, not real space between cells) —
// so there is no pixel left to start Qt's own rubber band from.
class LibraryGrid : public QListWidget {
public:
  using QListWidget::QListWidget;
  using QListWidget::setViewportMargins;

protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      drag_origin_ = event->pos();
      tracking_drag_ = true;
    }
    QListWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (tracking_drag_ && (event->buttons() & Qt::LeftButton)) {
      if (rubber_band_ == nullptr) {
        constexpr int kDragThreshold = 6;
        if ((event->pos() - drag_origin_).manhattanLength() < kDragThreshold) {
          QListWidget::mouseMoveEvent(event);
          return;
        }
        // A fresh drag replaces the selection unless it started with a
        // modifier held, matching plain-click behavior; either way, what's
        // selected right now (including whatever the initiating press
        // already selected) is the additive floor a shrinking rect won't
        // clear again below.
        if (!(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
          clearSelection();
        }
        base_selection_.clear();
        for (QListWidgetItem* selected : selectedItems()) base_selection_.insert(selected);
        rubber_band_ = new QRubberBand(QRubberBand::Rectangle, viewport());
        rubber_band_->setGeometry(QRect(drag_origin_, QSize()));
        rubber_band_->show();
      }
      const QRect rect = QRect(drag_origin_, event->pos()).normalized();
      rubber_band_->setGeometry(rect);
      for (int row = 0; row < count(); ++row) {
        QListWidgetItem* it = item(row);
        it->setSelected(base_selection_.contains(it) || rect.intersects(visualItemRect(it)));
      }
      return;
    }
    QListWidget::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    tracking_drag_ = false;
    if (rubber_band_ != nullptr) {
      rubber_band_->deleteLater();
      rubber_band_ = nullptr;
      base_selection_.clear();
      return;  // the drag already applied the selection; not a click
    }
    QListWidget::mouseReleaseEvent(event);
  }

private:
  QPoint drag_origin_;
  bool tracking_drag_ = false;
  QRubberBand* rubber_band_ = nullptr;
  QSet<QListWidgetItem*> base_selection_;
};

namespace {

// The top bar's filter picker. Status keys match mirad's `status` values;
// "all", "running" and "never" are frontend-only groupings.
struct FilterEntry {
  const char* label;
  const char* key;
};

const FilterEntry kFilters[] = {
    {"All games", "all"},
    {"Playing now", "running"},
    {"Ready", "ready"},
    {"Needs install", "needs_install"},
    {"Setting up", "setting_up"},
    {"Broken", "broken"},
    {"Missing", "missing"},
    {"Never played", "never"},
    // Every entry above excludes a hidden-tagged game; this is the only one
    // that shows them, and only them.
    {"Hidden", "hidden"},
};

bool HasTag(const mira_gui::GameSummary& game, const std::string& tag) {
  return std::find(game.tags.begin(), game.tags.end(), tag) != game.tags.end();
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

  details_ = new mira_gui::GameDetailsPanel(this);
  details_->SetArtworkStore(artwork_);
  connect(details_, &mira_gui::GameDetailsPanel::PlayRequested, this,
          [this](const QString& id) { LaunchGame(id.toStdString()); });
  connect(details_, &mira_gui::GameDetailsPanel::StopRequested, this,
          [this](const QString& id) { mira_gui::actions::Stop(this, id.toStdString()); });
  connect(details_, &mira_gui::GameDetailsPanel::EditRequested, this,
          [this](const QString& id) { OpenGameDialog(id.toStdString()); });
  connect(details_, &mira_gui::GameDetailsPanel::MetadataRefreshRequested, this,
          [this](const QString& id) { RefreshMetadata(id.toStdString()); });
  connect(details_, &mira_gui::GameDetailsPanel::ArtworkPickRequested, this,
          [this](const QString& id, const QString& slot) {
            OpenArtworkPicker(id.toStdString(), slot.toStdString());
          });
  connect(details_, &mira_gui::GameDetailsPanel::MoreDetailsRequested, this,
          [this](const QString& id) { OpenGameDetailPage(id.toStdString()); });

  // page 1 (game_edit_page_) is built lazily by OpenGameDialog.
  sidebar_stack_ = new QStackedWidget(this);
  sidebar_stack_->addWidget(details_);

  splitter_ = new QSplitter(Qt::Horizontal, this);
  splitter_->addWidget(BuildGrid());
  splitter_->addWidget(sidebar_stack_);
  splitter_->setStretchFactor(0, 1);
  splitter_->setStretchFactor(1, 0);
  splitter_->setSizes({850, 330});
  splitter_->setChildrenCollapsible(false);

  // page 1 (settings) is built lazily by OpenSettings and covers this
  // entire slot, sidebar included.
  content_stack_ = new QStackedWidget(this);
  content_stack_->addWidget(splitter_);

  auto* central = new RootWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->setContentsMargins(kResizeMargin, kResizeMargin, kResizeMargin, kResizeMargin);
  layout->setSpacing(0);
  QWidget* top_bar = BuildTopBar();
  // Without an explicit cursor here, a resize cursor RootWidget set at its
  // edge margin would keep showing over the whole window after the drag ends.
  top_bar->setCursor(Qt::ArrowCursor);
  layout->addWidget(top_bar);
  content_stack_->setCursor(Qt::ArrowCursor);
  layout->addWidget(content_stack_, /*stretch=*/1);

  footer_ = new QLabel(central);
  footer_->setProperty("role", "muted");
  footer_->setContentsMargins(10, 4, 10, 4);
  footer_->setCursor(Qt::ArrowCursor);
  layout->addWidget(footer_);

  setCentralWidget(central);

  // After BuildShortcuts, not before: BuildMenus reads common_'s actions,
  // which BuildShortcuts is what populates.
  BuildShortcuts();
  BuildMenus();

  LoadPrefs();
  RefreshHealth(/*force_scan=*/false);

  event_stream_.Start(this,
                      [this](std::string type, std::string data) { HandleGameEvent(type, data); });
}

void LibraryWindow::BuildMenus() {
  auto* menu = new QMenu(menu_button_);
  menu_button_->setMenu(menu);

  // Actions come from BuildShortcuts, already added to the window. Listing
  // one in a menu makes its key discoverable — Qt draws the sequence next
  // to the label.
  auto* file_menu = menu->addMenu("&File");
  file_menu->addAction(common_.close_window);
  file_menu->addAction(common_.quit);

  auto* view_menu = menu->addMenu("&View");
  // Always scans, whatever scan_on_startup says: the preference is about
  // opening the window, not about this command.
  QAction* refresh = view_menu->addAction("&Refresh library", this,
                                          [this] { RefreshHealth(/*force_scan=*/true); });
  // F5 is the platform's own Refresh; Ctrl+R is the one every browser
  // taught, and a second binding costs nothing. Shared id with MainWindow's
  // own refresh action -- editing either in Settings updates both.
  refresh->setShortcuts({mira_gui::keybindings::Register(refresh, "refresh", "Refresh the library",
                                                          QKeySequence(QKeySequence::Refresh),
                                                          {QKeySequence(Qt::CTRL | Qt::Key_R)}),
                        QKeySequence(Qt::CTRL | Qt::Key_R)});
  view_menu->addAction("Open &classic table view", this, &LibraryWindow::OpenClassicView);

  auto* library_menu = menu->addMenu("&Library");
  library_menu->addAction("Import &Steam library", this, &LibraryWindow::ImportSteamLibrary);
  library_menu->addAction("Import &Lutris games", this, &LibraryWindow::ImportLutrisLibrary)
      ->setToolTip(
          "Read Lutris's own database and add its Wine games here. Nothing is moved or renamed, "
          "in either launcher's files — a game stays playable in Lutris too.");
  library_menu->addAction("Fetch missing &cover art", this, &LibraryWindow::FetchMissingArtwork)
      ->setToolTip(
          "Re-fetch metadata for every game with no cover. mirad only fetches automatically for a "
          "newly detected game, so a game that failed once — or a non-Steam game from before a "
          "SteamGridDB key was set — stays without one until asked again.");
  library_menu->addSeparator();
  library_menu->addAction("Regenerate desktop entries", this, &LibraryWindow::SyncDesktopEntries)
      ->setToolTip(
          "Rewrites Mira's own mira-<id>.desktop entries immediately, without waiting for the "
          "next library change to pick up a desktop_entries.* setting edit.");
  library_menu->addAction("Remove all desktop entries…", this,
                          &LibraryWindow::RemoveAllDesktopEntries)
      ->setToolTip("Turns off desktop entries and deletes every one Mira generated.");

  auto* tools_menu = menu->addMenu("&Tools");
  tools_menu->addAction("&Runners…", this, &LibraryWindow::OpenRunners);
  QAction* settings = tools_menu->addAction("&Settings…", this, [this] { OpenSettings(); });
  // Spelled out rather than QKeySequence::Preferences, which Qt binds on
  // macOS only — this row showed no shortcut at all on Linux. Shared id with
  // MainWindow's own settings action.
  settings->setShortcut(mira_gui::keybindings::Register(settings, "settings", "Settings",
                                                         QKeySequence(Qt::CTRL | Qt::Key_Comma)));

  auto* help_menu = menu->addMenu("&Help");
  help_menu->addAction(common_.reference);
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
                {"Alt+Enter", "Details && settings"},
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
    connect(action, &QAction::triggered, this, [this, row] { filters_->setCurrentIndex(row); });
    addAction(action);
  }

  // A dedicated toggle for Hidden, on top of whatever Ctrl+9 already gives
  // it. Toggles back to All on a second press so it never strands the grid.
  window_action("toggle_hidden", "Toggle the Hidden filter", QKeySequence(Qt::CTRL | Qt::Key_H), {},
               [this] {
                 const int hidden_row = FilterRow("hidden");
                 if (hidden_row < 0) return;
                 filters_->setCurrentIndex(CurrentFilterKey() == "hidden" ? FilterRow("all")
                                                                          : hidden_row);
               });

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

  grid_action("details_settings", "Details && settings", QKeySequence(Qt::ALT | Qt::Key_Return),
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
    if (prefs.details_width) {
      const int details = *prefs.details_width;
      splitter_->setSizes({qMax(200, width() - details), details});
    }
    if (prefs.scan_on_startup) scan_on_startup_ = *prefs.scan_on_startup;
    if (prefs.notification_timeout_s) {
      mira_gui::notify::SetTimeoutSeconds(*prefs.notification_timeout_s);
    }
    if (prefs.shortcut_overrides) mira_gui::keybindings::LoadOverrides(*prefs.shortcut_overrides);
    if (prefs.sort_descending) {
      sort_descending_ = *prefs.sort_descending;
      sort_direction_->setArrowType(sort_descending_ ? Qt::DownArrow : Qt::UpArrow);
    }
    if (prefs.sort_by) {
      const int index = sort_->findData(QString::fromStdString(*prefs.sort_by));
      // An unknown key (hand-edited, or from a newer build) leaves the
      // picker where it is rather than selecting nothing.
      if (index >= 0) sort_->setCurrentIndex(index);
    }
    if (prefs.library_filter) {
      const QString wanted = QString::fromStdString(*prefs.library_filter);
      const int index = filters_->findData(wanted);
      if (index >= 0) filters_->setCurrentIndex(index);
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
  // Read back from notify rather than from a member, so a change made in
  // the settings dialog survives closing the window that did not make it.
  prefs.notification_timeout_s = mira_gui::notify::CurrentTimeoutSeconds();
  const QList<int> sizes = splitter_->sizes();
  if (sizes.size() == 2) prefs.details_width = sizes[1];
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
  menu_button_->setIcon(mira_gui::icons::For(Glyph::Menu));
  settings_button_->setIcon(mira_gui::icons::For(Glyph::Settings));
  minimize_button_->setIcon(mira_gui::icons::For(Glyph::Minimize));
  maximize_button_->setIcon(
      mira_gui::icons::For(isMaximized() ? Glyph::Restore : Glyph::Maximize));
  close_button_->setIcon(mira_gui::icons::For(Glyph::Close));
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
  const bool game_dirty = game_edit_form_ != nullptr &&
                          sidebar_stack_->currentWidget() == game_edit_page_ &&
                          game_edit_form_->IsDirty();
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
    mira_gui::notify::Toast(
        this, result.added > 0 ? mira_gui::notify::Level::Success : mira_gui::notify::Level::Info,
        QString("Scan: %1 added, %2 missing, %3 restored.")
            .arg(result.added)
            .arg(result.missing)
            .arg(result.restored));
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
    // A toast, not a popup: the import already happened, there is nothing to
    // decide, and the result is visible in the grid behind it either way.
    mira_gui::notify::Toast(
        this, result.added > 0 ? mira_gui::notify::Level::Success : mira_gui::notify::Level::Info,
        QString("Steam import: %1 added, %2 updated.").arg(result.added).arg(result.updated));
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
    QString message =
        QString("Lutris import: %1 added, %2 updated.").arg(result.added).arg(result.updated);
    // Worth saying: a skip is almost always a Steam-runner row or a game
    // whose prefix Lutris never wrote down, not a failure.
    if (result.skipped > 0) {
      message += QString(" %1 skipped (not a Wine game, or no prefix recorded).")
                     .arg(result.skipped);
    }
    mira_gui::notify::Toast(
        this, result.added > 0 ? mira_gui::notify::Level::Success : mira_gui::notify::Level::Info,
        message);
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
    mira_gui::notify::Toast(this, mira_gui::notify::Level::Success, "Desktop entries regenerated.");
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
              mira_gui::notify::Toast(this, mira_gui::notify::Level::Success,
                                     "Desktop entries removed.");
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

  menu_button_ = new QToolButton(top_bar_);
  menu_button_->setPopupMode(QToolButton::InstantPopup);
  menu_button_->setAutoRaise(true);
  // Its menu is filled in later, by BuildMenus() — deferred until
  // BuildShortcuts() has populated common_, which BuildMenus() reads.
  layout->addWidget(menu_button_);

  filters_ = new QComboBox(top_bar_);
  for (const FilterEntry& entry : kFilters) filters_->addItem(entry.label, QString(entry.key));
  connect(filters_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] { ApplyFilter(); });
  layout->addWidget(filters_);

  sort_ = new QComboBox(top_bar_);
  for (const mira_gui::SortOption& option : mira_gui::SortOptions()) {
    sort_->addItem(option.label, QString(option.key));
  }
  connect(sort_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
    sort_key_ = sort_->currentData().toString().toStdString();
    ApplyFilter();
  });
  layout->addWidget(sort_);

  sort_direction_ = new QToolButton(top_bar_);
  sort_direction_->setArrowType(Qt::UpArrow);
  sort_direction_->setAutoRaise(true);
  sort_direction_->setToolTip("Ascending — click for descending");
  connect(sort_direction_, &QToolButton::clicked, this, [this] {
    sort_descending_ = !sort_descending_;
    sort_direction_->setArrowType(sort_descending_ ? Qt::DownArrow : Qt::UpArrow);
    sort_direction_->setToolTip(sort_descending_ ? "Descending — click for ascending"
                                                 : "Ascending — click for descending");
    ApplyFilter();
  });
  layout->addWidget(sort_direction_);

  layout->addStretch(1);

  add_games_ = new QToolButton(top_bar_);
  add_games_->setObjectName("add_games");
  add_games_->setText("Add Games");
  add_games_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  add_games_->setPopupMode(QToolButton::InstantPopup);
  add_games_->setAutoRaise(true);
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

  search_ = new QLineEdit(top_bar_);
  search_->setObjectName("library_search");
  search_->setPlaceholderText("Search…");
  search_->setClearButtonEnabled(true);
  search_->setMaximumWidth(420);
  connect(search_, &QLineEdit::textChanged, this, [this] { ApplyFilter(); });
  layout->addWidget(search_, /*stretch=*/1);

  layout->addStretch(1);

  zoom_ = new QSlider(Qt::Horizontal, top_bar_);
  zoom_->setRange(120, 260);
  zoom_->setValue(tile_width_);
  zoom_->setMaximumWidth(120);
  zoom_->setToolTip("Tile size");
  connect(zoom_, &QSlider::valueChanged, this, &LibraryWindow::SetTileWidth);
  layout->addWidget(zoom_);

  // Gear <-> Back/Save, swapped by OpenSettings/CloseSettings. Two sibling
  // widgets, not a QStackedWidget — a stack is as wide as its widest page.
  settings_button_ = new QToolButton(top_bar_);
  settings_button_->setAutoRaise(true);
  settings_button_->setToolTip("Settings");
  connect(settings_button_, &QToolButton::clicked, this, [this] { OpenSettings(); });
  layout->addWidget(settings_button_);

  settings_actions_widget_ = new QWidget(top_bar_);
  auto* settings_actions_layout = new QHBoxLayout(settings_actions_widget_);
  settings_actions_layout->setContentsMargins(0, 0, 0, 0);
  settings_actions_layout->setSpacing(6);
  settings_back_button_ = new QPushButton("← Back", settings_actions_widget_);
  connect(settings_back_button_, &QPushButton::clicked, this, &LibraryWindow::RequestCloseSettings);
  settings_reset_button_ = new QPushButton("Reset", settings_actions_widget_);
  settings_reset_button_->setToolTip("Discard unsaved changes on this screen — back to what was last saved.");
  connect(settings_reset_button_, &QPushButton::clicked, this, [this] {
    if (settings_panel_ != nullptr) settings_panel_->DiscardChanges();
  });
  settings_save_button_ = new QPushButton("Save", settings_actions_widget_);
  connect(settings_save_button_, &QPushButton::clicked, this, [this] {
    if (settings_panel_ != nullptr) settings_panel_->Save();
  });
  settings_actions_layout->addWidget(settings_back_button_);
  settings_actions_layout->addWidget(settings_reset_button_);
  settings_actions_layout->addWidget(settings_save_button_);
  settings_actions_widget_->hide();
  layout->addWidget(settings_actions_widget_);

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

void LibraryWindow::SelectGridItem(const std::string& id) {
  for (int row = 0; row < grid_->count(); ++row) {
    QListWidgetItem* item = grid_->item(row);
    if (item->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString() != id) continue;
    grid_->setCurrentItem(item);
    return;
  }
}

void LibraryWindow::UpdateTileCover(const QString& id) {
  // One item, not a rebuild: artwork arrives one game at a time, and
  // ApplyFilter would drop the selection and scroll position on each.
  for (int row = 0; row < grid_->count(); ++row) {
    QListWidgetItem* item = grid_->item(row);
    if (item->data(mira_gui::GameTileDelegate::IdRole).toString() != id) continue;
    const mira_gui::GameSummary* game = FindGame(id.toStdString());
    if (game == nullptr) return;
    item->setData(Qt::DecorationRole, CoverFor(*game));
    // The panel draws the same game at a different size, so it needs the
    // same nudge — it has no way to notice the store changed under it.
    if (selected_id_ == game->id) details_->RefreshCover(*game);
    return;
  }
}

void LibraryWindow::ShowSteamGridDbNotice(bool asked_for) {
  // Once per session, however many games report it.
  if (steamgriddb_notice_shown_) return;
  steamgriddb_notice_shown_ = true;

  if (!asked_for) {
    // Nobody asked for this; a modal over a background scan is an ambush.
    mira_gui::notify::Toast(
        this, mira_gui::notify::Level::Warning,
        "No SteamGridDB API key set — non-Steam games can't get cover art. See Settings.");
    return;
  }

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
          mira_gui::notify::Toast(this, mira_gui::notify::Level::Success,
                                  "Every game already has cover art.");
          return;
        }
        // One toast for the batch, not one notification per game.
        mira_gui::notify::Toast(
            this, mira_gui::notify::Level::Info,
            QString("Fetching cover art for %1 game(s)… they appear as they arrive.")
                .arg(result.count));
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
  const QVariant data = filters_->currentData();
  return data.isValid() ? data.toString() : QString("all");
}

int LibraryWindow::FilterRow(const QString& key) const {
  for (int row = 0; row < filters_->count(); ++row) {
    if (filters_->itemData(row).toString() == key) return row;
  }
  return -1;
}

bool LibraryWindow::MatchesFilter(const mira_gui::GameSummary& game) const {
  const QString search = search_->text().trimmed();
  if (!search.isEmpty() &&
      !QString::fromStdString(game.name).contains(search, Qt::CaseInsensitive)) {
    return false;
  }

  const QString filter = CurrentFilterKey();
  if (filter == "hidden") return HasTag(game, "hidden");
  // Every other filter excludes a hidden game — "not displayed by default"
  // means not in "All games" either, not just off the initial screen.
  if (HasTag(game, "hidden")) return false;
  if (filter == "all") return true;
  if (filter == "running") return running_ids_.contains(game.id);
  if (filter == "never") return !game.last_played_at.has_value();
  return filter.toStdString() == game.status;
}

void LibraryWindow::ApplyFilter() {
  const std::string previously_selected = selected_id_;

  // Sorted here, not at fetch time, so a sort change costs a tile rebuild,
  // not a round trip.
  mira_gui::SortGames(games_, sort_key_, sort_descending_);

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
    item->setToolTip(game.last_error.empty()
                         ? QString::fromStdString(game.name)
                         : QString("%1\n%2").arg(QString::fromStdString(game.name),
                                                 QString::fromStdString(game.last_error)));
    if (game.id == previously_selected) to_select = item;
  }
  grid_->blockSignals(false);

  if (to_select != nullptr) {
    grid_->setCurrentItem(to_select);
  } else if (!previously_selected.empty()) {
    // Selected game was filtered away or removed — don't keep showing it.
    selected_id_.clear();
    details_->Clear();
  }

  empty_hint_->setVisible(shown == 0);
  if (shown == 0) {
    empty_hint_->setText(games_.empty() ? "No games in the library yet."
                                        : "No games match this filter.");
  }

  footer_->setText(QString("%1 of %2 games · connected via %3")
                       .arg(shown)
                       .arg(games_.size())
                       .arg(QString::fromStdString(mira_gui::MiradClient::ResolveSocketPath())));
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
  if (content_stack_->currentWidget() == settings_page_) return;  // grid isn't on screen
  if (restoring_selection_) return;  // re-entrant call from the revert below

  if (sidebar_stack_->currentWidget() == game_edit_page_ &&
      !(game_edit_form_ != nullptr && game_edit_form_->IsDirty())) {
    sidebar_stack_->setCurrentWidget(details_);
  } else if (sidebar_stack_->currentWidget() == game_edit_page_) {
    QListWidgetItem* clicked = grid_->currentItem();
    const std::string clicked_id =
        clicked != nullptr ? clicked->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString()
                           : std::string();

    switch (mira_gui::notify::ConfirmUnsaved(this, "This game's edits aren't saved.")) {
      case mira_gui::notify::UnsavedAction::Cancel:
        restoring_selection_ = true;
        SelectGridItem(game_edit_form_->id());
        restoring_selection_ = false;
        return;
      case mira_gui::notify::UnsavedAction::SaveAndExit:
        // So the reselect RefreshGames does after CloseGameEdit() (via the
        // SaveFinished connection in BuildGameEditPage) lands on the tile
        // just clicked, not the one being edited.
        selected_id_ = clicked_id;
        game_edit_form_->Save();
        return;
      case mira_gui::notify::UnsavedAction::DiscardAndExit:
        sidebar_stack_->setCurrentWidget(details_);
        break;
    }
  }

  const QList<QListWidgetItem*> selected = grid_->selectedItems();
  if (selected.size() > 1) {
    // More than one game selected: nothing single-game (cover, play button,
    // per-game metadata) makes sense here, so the panel switches to a plain
    // summary instead of just showing whichever one happens to be "current".
    selected_id_.clear();
    QStringList names;
    names.reserve(selected.size());
    for (QListWidgetItem* selected_item : selected) {
      names << selected_item->data(mira_gui::GameTileDelegate::NameRole).toString();
    }
    details_->ShowMultiSelection(names);
    return;
  }

  auto* item = grid_->currentItem();
  const mira_gui::GameSummary* game =
      item != nullptr && item->isSelected()
          ? FindGame(item->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString())
          : nullptr;
  if (game == nullptr) {
    selected_id_.clear();
    details_->Clear();
    return;
  }
  selected_id_ = game->id;
  details_->ShowGame(*game, running_ids_.contains(game->id));
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
  QAction* details = menu.addAction("Details && settings…");
  QAction* folder = menu.addAction("Open install folder");
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
    for (const std::string& game_id : ids) RefreshMetadata(game_id, /*announce=*/false);
    mira_gui::notify::Toast(this, mira_gui::notify::Level::Info,
                            QString("Refreshing metadata for %1 game(s)…").arg(count));
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
  if (game_edit_page_ != nullptr) {
    sidebar_stack_->removeWidget(game_edit_page_);
    game_edit_page_->deleteLater();
  }
  game_edit_page_ = BuildGameEditPage(id);
  sidebar_stack_->addWidget(game_edit_page_);
  sidebar_stack_->setCurrentWidget(game_edit_page_);
}

void LibraryWindow::CloseGameEdit() {
  sidebar_stack_->setCurrentWidget(details_);
  // Torn down rather than left alive off-screen: IsDirty() on a discarded
  // form would otherwise still read dirty, and wrongly prompt again on the
  // next Ctrl+Q from the grid.
  if (game_edit_page_ != nullptr) {
    sidebar_stack_->removeWidget(game_edit_page_);
    game_edit_page_->deleteLater();
    game_edit_page_ = nullptr;
    game_edit_form_ = nullptr;
  }
  RefreshGames();
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
  settings_button_->setVisible(!settings_open);
  settings_actions_widget_->setVisible(settings_open);
  // These act on a grid that isn't on screen while settings covers it.
  for (QWidget* control : {static_cast<QWidget*>(filters_), static_cast<QWidget*>(sort_),
                           static_cast<QWidget*>(sort_direction_), static_cast<QWidget*>(add_games_),
                           static_cast<QWidget*>(search_), static_cast<QWidget*>(zoom_)}) {
    control->setEnabled(!settings_open);
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
          });
  layout->addWidget(settings_panel_, /*stretch=*/1);

  return page;
}

QWidget* LibraryWindow::BuildGameEditPage(const std::string& id) {
  auto* page = new QWidget(this);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(10);

  auto* save = new QPushButton("Save", page);

  auto* scroll = new QScrollArea(page);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  game_edit_form_ = new mira_gui::GameEditForm(id, scroll);
  game_edit_form_->SetArtworkStore(artwork_);
  connect(save, &QPushButton::clicked, game_edit_form_, &mira_gui::GameEditForm::Save);
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
            // The screen closing back to the grid is already the feedback —
            // a save the user just triggered isn't the background-result
            // case a toast is for.
            CloseGameEdit();
          });
  scroll->setWidget(game_edit_form_);
  layout->addWidget(scroll, /*stretch=*/1);

  auto* footer = new QHBoxLayout();
  auto* back = new QPushButton("← Back", page);
  connect(back, &QPushButton::clicked, this, [this] {
    if (game_edit_form_ == nullptr || !game_edit_form_->IsDirty()) {
      CloseGameEdit();
      return;
    }
    switch (mira_gui::notify::ConfirmUnsaved(this, "This game's edits aren't saved.")) {
      case mira_gui::notify::UnsavedAction::Cancel:
        return;
      case mira_gui::notify::UnsavedAction::SaveAndExit:
        game_edit_form_->Save();  // SaveFinished, connected above, closes on success
        return;
      case mira_gui::notify::UnsavedAction::DiscardAndExit:
        CloseGameEdit();
        return;
    }
  });
  footer->addWidget(back);
  footer->addWidget(save);
  footer->addStretch(1);
  layout->addLayout(footer);

  return page;
}

void LibraryWindow::OpenClassicView() {
  // A second top-level window, not a swap: the two views are useful side
  // by side, and both stay live since each holds its own EventStream.
  auto* classic = new MainWindow();
  classic->setAttribute(Qt::WA_DeleteOnClose);
  classic->setWindowTitle("Mira — classic view");
  classic->show();
}

void LibraryWindow::HandleGameEvent(const std::string& type, const std::string& data) {
  if (type == "notification") {
    mira_gui::NotificationEvent event;
    if (mira_gui::MiradClient::ParseNotification(data, &event)) {
      mira_gui::notify::Toast(this, mira_gui::notify::LevelFromString(QString::fromStdString(event.level)),
                              QString::fromStdString(event.message));
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
      ShowSteamGridDbNotice(awaiting_metadata_.erase(event.id) > 0);
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
      details_->RefreshBanner(event.id);
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
