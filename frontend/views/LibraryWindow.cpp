#include "LibraryWindow.h"

#include <QAbstractItemView>
#include <QAction>
#include <QKeySequence>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QCloseEvent>
#include <QComboBox>
#include <QMenuBar>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

#include <iterator>

#include "../client/MiradClient.h"
#include "../dialogs/GameDetailDialog.h"
#include "../dialogs/RunnerDialog.h"
#include "../dialogs/SettingsDialog.h"
#include "../ui/CoverArt.h"
#include "../ui/GameActions.h"
#include "../ui/GameDetailsPanel.h"
#include "../ui/GameTileDelegate.h"
#include "../ui/LibrarySort.h"
#include "../ui/Notify.h"
#include "../ui/Shortcuts.h"
#include "MainWindow.h"

namespace {

// Sidebar entries. The status keys match docs/api.md's `status` values
// exactly; "all", "running" and "never" are frontend-only groupings.
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
};

}  // namespace

LibraryWindow::LibraryWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("Mira");
  resize(1180, 720);

  // Before the panel and the grid, because both ask it for covers.
  artwork_ = new mira_gui::ArtworkStore(this);
  connect(artwork_, &mira_gui::ArtworkStore::CoverChanged, this, &LibraryWindow::UpdateTileCover);

  details_ = new mira_gui::GameDetailsPanel(this);
  details_->SetArtworkStore(artwork_);
  connect(details_, &mira_gui::GameDetailsPanel::PlayRequested, this,
          [this](const QString& id) { LaunchGame(id.toStdString()); });
  connect(details_, &mira_gui::GameDetailsPanel::StopRequested, this,
          [this](const QString& id) { mira_gui::actions::Stop(this, id.toStdString()); });
  connect(details_, &mira_gui::GameDetailsPanel::EditRequested, this,
          [this](const QString& id) { OpenGameDialog(id.toStdString()); });

  splitter_ = new QSplitter(Qt::Horizontal, this);
  splitter_->addWidget(BuildSidebar());
  splitter_->addWidget(BuildGrid());
  splitter_->addWidget(details_);
  splitter_->setStretchFactor(0, 0);
  splitter_->setStretchFactor(1, 1);
  splitter_->setStretchFactor(2, 0);
  splitter_->setSizes({190, 660, 330});
  splitter_->setChildrenCollapsible(false);

  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(splitter_, /*stretch=*/1);

  footer_ = new QLabel(central);
  footer_->setStyleSheet("font-size: 10px; color: #9e9e9e; padding: 4px 10px;");
  layout->addWidget(footer_);

  setCentralWidget(central);

  // After the widgets, because most of these act on one: BuildMenus
  // then puts the three window-wide actions into File and Help.
  BuildShortcuts();
  BuildMenus();

  LoadPrefs();
  RefreshHealth(/*force_scan=*/false);

  event_stream_.Start(this,
                      [this](std::string type, std::string data) { HandleGameEvent(type, data); });
}

void LibraryWindow::BuildMenus() {
  // The actions themselves come from BuildShortcuts, which already added
  // them to the window. Listing one in a menu is what makes its key
  // discoverable — Qt draws the sequence next to the label.
  auto* file_menu = menuBar()->addMenu("&File");
  file_menu->addAction(common_.close_window);
  file_menu->addAction(common_.quit);

  auto* view_menu = menuBar()->addMenu("&View");
  // Always scans, whatever scan_on_startup says: the preference is about
  // opening the window, not about this command.
  QAction* refresh = view_menu->addAction("&Refresh library", this,
                                          [this] { RefreshHealth(/*force_scan=*/true); });
  // F5 is the platform's own Refresh; Ctrl+R is the one every browser
  // taught, and a second binding costs nothing.
  refresh->setShortcuts({QKeySequence(QKeySequence::Refresh), QKeySequence(Qt::CTRL | Qt::Key_R)});
  view_menu->addSeparator();
  view_menu->addAction("Open &classic table view", this, &LibraryWindow::OpenClassicView);

  auto* library_menu = menuBar()->addMenu("&Library");
  library_menu->addAction("Import &Steam library", this, &LibraryWindow::ImportSteamLibrary);

  auto* tools_menu = menuBar()->addMenu("&Tools");
  tools_menu->addAction("&Runners…", this, &LibraryWindow::OpenRunners);
  QAction* settings = tools_menu->addAction("&Settings…", this, &LibraryWindow::OpenSettings);
  // Spelled out rather than QKeySequence::Preferences, which Qt binds on
  // macOS only — this row showed no shortcut at all on Linux.
  settings->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));

  auto* help_menu = menuBar()->addMenu("&Help");
  help_menu->addAction(common_.reference);
}

void LibraryWindow::BuildShortcuts() {
  common_ = mira_gui::shortcuts::Install(
      this, {
                {"Ctrl+F", "Focus the search box"},
                {"Esc", "Clear the search, then the selection"},
                {"Ctrl+1…8", "Pick a sidebar filter"},
                {"F5, Ctrl+R", "Refresh the library"},
                {"Enter", "Play the selected game — Stop while it runs"},
                {"Alt+Enter", "Details & settings"},
                {"Delete", "Remove the selected game"},
                {"Ctrl++, Ctrl+-", "Tile size"},
                {"Ctrl+0", "Reset tile size"},
                {"Ctrl+,", "Settings"},
            });

  auto window_action = [this](std::initializer_list<QKeySequence> keys, auto slot) {
    auto* action = new QAction(this);
    action->setShortcuts(QList<QKeySequence>(keys));
    connect(action, &QAction::triggered, this, slot);
    addAction(action);
  };

  // Scoped to the grid, not to the window: Delete and Enter still have to
  // mean what they mean inside the search box, and WidgetWithChildrenShortcut
  // is what keeps a keystroke aimed at a text field from reaching the
  // library instead.
  auto grid_action = [this](std::initializer_list<QKeySequence> keys, auto slot) {
    auto* action = new QAction(grid_);
    action->setShortcuts(QList<QKeySequence>(keys));
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(action, &QAction::triggered, this, slot);
    grid_->addAction(action);
  };

  window_action({QKeySequence(QKeySequence::Find)}, [this] {
    search_->setFocus(Qt::ShortcutFocusReason);
    search_->selectAll();
  });

  // One key, two jobs, in the order a user expects to undo them: the search
  // narrowed the library, so it goes first, and only an already-empty box
  // means Escape was aimed at the selection.
  window_action({QKeySequence(Qt::Key_Escape)}, [this] {
    if (!search_->text().isEmpty()) {
      search_->clear();
      return;
    }
    grid_->clearSelection();
    grid_->setCurrentItem(nullptr);
  });

  window_action({QKeySequence(QKeySequence::ZoomIn), QKeySequence(Qt::CTRL | Qt::Key_Equal)},
                [this] { zoom_->setValue(zoom_->value() + zoom_->pageStep()); });
  window_action({QKeySequence(QKeySequence::ZoomOut)},
                [this] { zoom_->setValue(zoom_->value() - zoom_->pageStep()); });
  window_action({QKeySequence(Qt::CTRL | Qt::Key_0)},
                [this] { zoom_->setValue(kDefaultTileWidth); });

  // Ctrl+1 through Ctrl+8, in sidebar order. Guarded by count() rather than
  // by kFilters so adding a ninth filter cannot walk past Ctrl+9.
  for (int row = 0; row < filters_->count() && row < 9; ++row) {
    window_action({QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + row))},
                  [this, row] { filters_->setCurrentRow(row); });
  }

  // Qt::Key_Enter is the keypad one — a separate key from Qt::Key_Return,
  // and binding only Return would leave it dead.
  grid_action({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)}, [this] {
    const mira_gui::GameSummary* game = FindGame(selected_id_);
    if (game == nullptr) return;
    // The same rule the context menu's Play entry enforces: a game that is
    // not ready has nothing to launch, and Enter does not get to be the one
    // path that ignores that.
    if (!running_ids_.contains(game->id) && game->status != "ready") return;
    ToggleRunning(std::string(game->id));
  });

  grid_action({QKeySequence(Qt::ALT | Qt::Key_Return), QKeySequence(Qt::ALT | Qt::Key_Enter)},
              [this] {
                if (selected_id_.empty()) return;
                OpenGameDialog(std::string(selected_id_));
              });

  grid_action({QKeySequence(Qt::Key_Delete)}, [this] {
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
      // Through the slider so the clamp to its range and SetTileWidth's
      // cache invalidation both apply — a hand-edited frontend.toml must
      // not be able to ask for a 4000px tile.
      zoom_->setValue(*prefs.tile_width);
    }
    if (prefs.sidebar_width && prefs.details_width) {
      const int middle = qMax(200, width() - *prefs.sidebar_width - *prefs.details_width);
      splitter_->setSizes({*prefs.sidebar_width, middle, *prefs.details_width});
    }
    if (prefs.scan_on_startup) scan_on_startup_ = *prefs.scan_on_startup;
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
      for (int row = 0; row < filters_->count(); ++row) {
        if (filters_->item(row)->data(Qt::UserRole).toString() == wanted) {
          filters_->setCurrentRow(row);
          break;
        }
      }
    }
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
  if (sizes.size() == 3) {
    prefs.sidebar_width = sizes[0];
    prefs.details_width = sizes[2];
  }
  // Blocking, not fire-and-forget: this runs from closeEvent, and on the
  // last window the process exits before a worker thread ever reaches the
  // socket. A failure here costs a remembered layout, not data, so the
  // result is still not worth reporting — but losing the write to a race
  // was not a trade-off, it was a bug.
  // Blocking, not fire-and-forget. The async form hands the request to a
  // detached thread (async::Run), and this is the one call site where the
  // process may exit before that thread reaches the socket — closeEvent on
  // the last window is immediately followed by exec() returning. The race
  // is normally won, and every attempt to lose it here did win, but
  // "usually saves your layout" is not what a Quit key should promise. A
  // failure is still not worth reporting: the cost is a remembered layout,
  // not data.
  mira_gui::MiradClient::SaveFrontendPrefsBlocking(prefs);
}

void LibraryWindow::closeEvent(QCloseEvent* event) {
  SavePrefs();
  QMainWindow::closeEvent(event);
}

void LibraryWindow::OpenRunners() {
  RunnerDialog dialog(this);
  dialog.exec();
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

QWidget* LibraryWindow::BuildSidebar() {
  auto* sidebar = new QWidget(this);
  auto* layout = new QVBoxLayout(sidebar);
  layout->setContentsMargins(10, 10, 6, 10);
  layout->setSpacing(8);

  auto* title = new QLabel("Mira", sidebar);
  title->setStyleSheet("font-size: 18px; font-weight: 600;");
  layout->addWidget(title);

  health_badge_ = new QLabel(sidebar);
  health_badge_->setStyleSheet("font-size: 11px; color: #757575;");
  health_badge_->setText("● checking…");
  layout->addWidget(health_badge_);

  search_ = new QLineEdit(sidebar);
  search_->setObjectName("library_search");
  search_->setPlaceholderText("Search…");
  search_->setClearButtonEnabled(true);
  connect(search_, &QLineEdit::textChanged, this, [this] { ApplyFilter(); });
  layout->addWidget(search_);

  filters_ = new QListWidget(sidebar);
  filters_->setFrameShape(QFrame::NoFrame);
  for (const FilterEntry& entry : kFilters) {
    auto* item = new QListWidgetItem(entry.label, filters_);
    item->setData(Qt::UserRole, QString(entry.key));
  }
  filters_->setCurrentRow(0);
  // Sized to its contents rather than stretched: a filter list with eight
  // fixed entries and a metre of empty space under them reads as a list that
  // failed to load.
  filters_->setFixedHeight(filters_->sizeHintForRow(0) * filters_->count() +
                           2 * filters_->frameWidth() + 4);
  connect(filters_, &QListWidget::currentRowChanged, this, [this] { ApplyFilter(); });
  layout->addWidget(filters_);
  layout->addStretch(1);

  auto* settings_button = new QPushButton("Settings", sidebar);
  connect(settings_button, &QPushButton::clicked, this, &LibraryWindow::OpenSettings);
  layout->addWidget(settings_button);

  return sidebar;
}

QWidget* LibraryWindow::BuildGrid() {
  auto* container = new QWidget(this);
  auto* layout = new QVBoxLayout(container);
  layout->setContentsMargins(6, 10, 6, 6);
  layout->setSpacing(6);

  auto* toolbar = new QHBoxLayout();
  auto* sort_label = new QLabel("Sort by", container);
  sort_label->setStyleSheet("font-size: 11px; color: #9e9e9e;");
  toolbar->addWidget(sort_label);

  sort_ = new QComboBox(container);
  for (const mira_gui::SortOption& option : mira_gui::SortOptions()) {
    sort_->addItem(option.label, QString(option.key));
  }
  connect(sort_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
    sort_key_ = sort_->currentData().toString().toStdString();
    ApplyFilter();
  });
  toolbar->addWidget(sort_);

  sort_direction_ = new QToolButton(container);
  sort_direction_->setArrowType(Qt::UpArrow);
  sort_direction_->setToolTip("Ascending — click for descending");
  connect(sort_direction_, &QToolButton::clicked, this, [this] {
    sort_descending_ = !sort_descending_;
    sort_direction_->setArrowType(sort_descending_ ? Qt::DownArrow : Qt::UpArrow);
    sort_direction_->setToolTip(sort_descending_ ? "Descending — click for ascending"
                                                 : "Ascending — click for descending");
    ApplyFilter();
  });
  toolbar->addWidget(sort_direction_);

  toolbar->addStretch(1);
  auto* zoom_label = new QLabel("Tile size", container);
  zoom_label->setStyleSheet("font-size: 11px; color: #9e9e9e;");
  toolbar->addWidget(zoom_label);
  zoom_ = new QSlider(Qt::Horizontal, container);
  zoom_->setRange(120, 260);
  zoom_->setValue(tile_width_);
  zoom_->setMaximumWidth(140);
  connect(zoom_, &QSlider::valueChanged, this, &LibraryWindow::SetTileWidth);
  toolbar->addWidget(zoom_);
  layout->addLayout(toolbar);

  grid_ = new QListWidget(container);
  grid_->setObjectName("library_grid");
  delegate_ = new mira_gui::GameTileDelegate(grid_, TileSize());
  grid_->setItemDelegate(delegate_);
  grid_->setViewMode(QListView::IconMode);
  grid_->setResizeMode(QListView::Adjust);
  grid_->setMovement(QListView::Static);
  grid_->setUniformItemSizes(true);
  grid_->setSpacing(0);
  grid_->setGridSize(TileSize());
  grid_->setSelectionMode(QAbstractItemView::SingleSelection);
  grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  grid_->setMouseTracking(true);
  grid_->setFrameShape(QFrame::NoFrame);
  grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  grid_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(grid_, &QListWidget::itemSelectionChanged, this, &LibraryWindow::SelectionChanged);
  connect(grid_, &QListWidget::customContextMenuRequested, this, &LibraryWindow::ShowContextMenu);
  connect(grid_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
    ToggleRunning(item->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString());
  });
  layout->addWidget(grid_, /*stretch=*/1);

  empty_hint_ = new QLabel(container);
  empty_hint_->setAlignment(Qt::AlignCenter);
  empty_hint_->setStyleSheet("color: #9e9e9e;");
  empty_hint_->setVisible(false);
  layout->addWidget(empty_hint_);

  return container;
}

QSize LibraryWindow::TileSize() const {
  // 2:3 portrait, the cover ratio Playnite (and every store front that feeds
  // it) uses, plus room for the title band drawn over the bottom.
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

void LibraryWindow::RefreshMetadata(const std::string& id) {
  mira_gui::MiradClient::RefreshMetadataAsync(
      this, id, [this, id](mira_gui::MetadataRefreshResult result) {
        if (!result.ok) {
          mira_gui::notify::Failed(this, "Could not refresh metadata.",
                                   QString::fromStdString(result.error));
          return;
        }
        // 202: the fetch runs on the daemon and reports back as an event.
        // Remembered so that its failure is worth a toast — see
        // HandleGameEvent, where an unasked-for failure is not.
        awaiting_metadata_.insert(id);
        mira_gui::notify::Toast(this, mira_gui::notify::Level::Info,
                                "Fetching metadata and cover art…");
      });
}

void LibraryWindow::SetHealthy(bool healthy, const QString& tooltip) {
  health_badge_->setToolTip(tooltip);
  if (healthy) {
    health_badge_->setText("● Online");
    health_badge_->setStyleSheet("font-size: 11px; color: #2e7d32; font-weight: 600;");
  } else {
    health_badge_->setText("● Offline");
    health_badge_->setStyleSheet("font-size: 11px; color: #c62828; font-weight: 600;");
  }
}

void LibraryWindow::RefreshHealth(bool force_scan) {
  health_badge_->setText("● checking…");
  health_badge_->setStyleSheet("font-size: 11px; color: #757575;");

  mira_gui::MiradClient::CheckHealthAsync(this, [this, force_scan](mira_gui::HealthStatus status) {
    SetHealthy(status.reachable, QString::fromStdString(status.detail));
    if (status.reachable) {
      RescanAndRefreshGames(force_scan);
    } else {
      games_.clear();
      ApplyFilter();
    }
  });
}

void LibraryWindow::RescanAndRefreshGames(bool force_scan) {
  if (!force_scan && !scan_on_startup_) {
    // The daemon's own watcher keeps the library current while it runs
    // (library::Watcher), so skipping the startup scan costs nothing except
    // on a library that changed while mirad was stopped.
    RefreshGames();
    return;
  }
  mira_gui::MiradClient::ScanLibraryAsync(this, [this](mira_gui::ScanResult) { RefreshGames(); });
}

void LibraryWindow::RefreshGames() {
  // No ?status= filter: the sidebar filters client-side over the whole
  // library (see ApplyFilter), so this is the only fetch either way.
  mira_gui::MiradClient::ListGamesAsync(this, [this](mira_gui::GamesResult result) {
    if (!result.ok) {
      health_badge_->setToolTip(QString("mirad is reachable, but GET /v1/games failed: %1")
                                    .arg(QString::fromStdString(result.error)));
      games_.clear();
      ApplyFilter();
      return;
    }
    games_ = std::move(result.games);
    ApplyFilter();
  });
}

QString LibraryWindow::CurrentFilterKey() const {
  auto* item = filters_->currentItem();
  return item ? item->data(Qt::UserRole).toString() : QString("all");
}

bool LibraryWindow::MatchesFilter(const mira_gui::GameSummary& game) const {
  const QString search = search_->text().trimmed();
  if (!search.isEmpty() &&
      !QString::fromStdString(game.name).contains(search, Qt::CaseInsensitive)) {
    return false;
  }

  const QString filter = CurrentFilterKey();
  if (filter == "all") return true;
  if (filter == "running") return running_ids_.contains(game.id);
  if (filter == "never") return !game.last_played_at.has_value();
  return filter.toStdString() == game.status;
}

void LibraryWindow::ApplyFilter() {
  const std::string previously_selected = selected_id_;

  // Sorted here rather than at fetch time so a sort change costs a rebuild
  // of the tiles and not a round trip — and so an event that patches one
  // game into games_ lands in the right place without re-fetching either.
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
    // The selected game was filtered away (or removed) — the panel would
    // otherwise keep showing a game that isn't on screen any more.
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
  // A rename changes the placeholder's initials, so every rendered tile for
  // this id is stale — the fetched artwork behind it is not, which is why
  // this drops the rendering and not the image.
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
  grid_->setCurrentItem(item);

  const std::string id = item->data(mira_gui::GameTileDelegate::IdRole).toString().toStdString();
  const QString name = item->data(mira_gui::GameTileDelegate::NameRole).toString();
  const std::string status =
      item->data(mira_gui::GameTileDelegate::StatusRole).toString().toStdString();
  const bool running = running_ids_.contains(id);

  QMenu menu(this);
  QAction* play = menu.addAction(running ? "Stop" : "Play");
  play->setEnabled(running || status == "ready");
  QAction* details = menu.addAction("Details && settings…");
  QAction* folder = menu.addAction("Open install folder");
  menu.addSeparator();
  // Both halves of the needs_install escape hatch (docs/api.md): run the
  // installer inside this game's prefix, then say it worked. Offered for
  // every game, since running something in a prefix is useful beyond
  // installing, but only a needs_install game can be "marked installed".
  QAction* run_in_prefix = menu.addAction("Run in prefix…");
  QAction* finish_install = menu.addAction("Mark as installed");
  finish_install->setEnabled(status == "needs_install");
  finish_install->setToolTip(status == "needs_install"
                                 ? "Flip this game to ready once its executable points at the "
                                   "installed program"
                                 : "Only applies to a game that still needs installing");
  QAction* refresh_metadata = menu.addAction("Refresh metadata && cover art");
  menu.addSeparator();
  QAction* remove = menu.addAction("Remove from library…");

  QAction* chosen = menu.exec(grid_->viewport()->mapToGlobal(pos));
  if (chosen == play) {
    ToggleRunning(id);
  } else if (chosen == details) {
    OpenGameDialog(id);
  } else if (chosen == folder) {
    mira_gui::actions::OpenInstallFolder(this, id);
  } else if (chosen == run_in_prefix) {
    mira_gui::actions::RunInPrefix(this, id);
  } else if (chosen == finish_install) {
    mira_gui::actions::FinishInstall(this, id, [this] { RefreshGames(); });
  } else if (chosen == refresh_metadata) {
    RefreshMetadata(id);
  } else if (chosen == remove) {
    mira_gui::actions::Delete(this, id, name, [this] { RefreshGames(); });
  }
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
    // never emits game.state, so marking it running here would pin it under
    // that filter with no event able to release it.
    if (tracked) running_ids_.insert(id);
    RefreshGames();
  });
}

void LibraryWindow::OpenGameDialog(const std::string& id) {
  GameDetailDialog dialog(id, this);
  dialog.exec();
  RefreshGames();
}

void LibraryWindow::OpenSettings() {
  SettingsDialog dialog(this);
  dialog.exec();
}

void LibraryWindow::OpenClassicView() {
  // A second top-level window rather than a swap: the two views are useful
  // side by side (audit a row in the table, watch the tile update here),
  // and both stay live because each holds its own EventStream.
  auto* classic = new MainWindow();
  classic->setAttribute(Qt::WA_DeleteOnClose);
  classic->setWindowTitle("Mira — classic view");
  classic->show();
}

void LibraryWindow::HandleGameEvent(const std::string& type, const std::string& data) {
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
    // An exit changes play_seconds/last_played_at, which `game.state` does
    // not carry (docs/api.md) — hence a re-fetch rather than a local patch.
    RefreshGames();
    return;
  }

  if (type == "game.metadata_ready" || type == "game.metadata_failed") {
    mira_gui::MetadataEvent event;
    if (!mira_gui::MiradClient::ParseMetadataEvent(data, &event)) return;
    if (type == "game.metadata_ready") {
      // Only the artwork is refetched here. The rest of the metadata is not
      // part of the game record (docs/api.md keeps it out of games.toml), so
      // nothing in the library view changes when it lands.
      artwork_->Invalidate(event.id);
      return;
    }
    // A failure is reported only for a game the user asked about. On a fresh
    // scan mirad fetches for every new game at once, and most of those
    // failures are "this isn't on Steam and there's no SteamGridDB key" —
    // one toast per game would bury the window.
    if (awaiting_metadata_.erase(event.id) > 0) {
      mira_gui::notify::Toast(this, mira_gui::notify::Level::Warning,
                              QString("No metadata found: %1")
                                  .arg(event.error.empty()
                                           ? QString("nothing matched this game")
                                           : QString::fromStdString(event.error)));
    }
    return;
  }

  if (type == "game.launched") {
    // The untracked counterpart to game.state (docs/api.md): mirad handed
    // this one to Steam and is not watching it. Clearing rather than
    // ignoring, because the launch may have come from elsewhere — the CLI,
    // the other window — that did mark it running.
    const std::string id = mira_gui::MiradClient::ParseRemovedId(data);
    if (!id.empty()) {
      running_ids_.erase(id);
      RefreshGames();
    }
    return;
  }

  mira_gui::GameSummary game;
  if (mira_gui::MiradClient::ParseGameSummary(data, &game)) UpsertGame(game);
}
