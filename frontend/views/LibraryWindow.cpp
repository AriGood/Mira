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
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
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

  BuildMenus();

  details_ = new mira_gui::GameDetailsPanel(this);
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

  LoadPrefs();
  RefreshHealth();

  event_stream_.Start(this,
                      [this](std::string type, std::string data) { HandleGameEvent(type, data); });
}

void LibraryWindow::BuildMenus() {
  auto* view_menu = menuBar()->addMenu("&View");
  view_menu->addAction("&Refresh library", QKeySequence::Refresh, this,
                       &LibraryWindow::RefreshHealth);
  view_menu->addSeparator();
  view_menu->addAction("Open &classic table view", this, &LibraryWindow::OpenClassicView);

  auto* library_menu = menuBar()->addMenu("&Library");
  library_menu->addAction("Import &Steam library", this, &LibraryWindow::ImportSteamLibrary);

  auto* tools_menu = menuBar()->addMenu("&Tools");
  tools_menu->addAction("&Runners…", this, &LibraryWindow::OpenRunners);
  tools_menu->addAction("&Settings…", QKeySequence::Preferences, this,
                        &LibraryWindow::OpenSettings);
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
  const QList<int> sizes = splitter_->sizes();
  if (sizes.size() == 3) {
    prefs.sidebar_width = sizes[0];
    prefs.details_width = sizes[2];
  }
  // Fire-and-forget: the window is closing, and a failure here costs a
  // remembered layout, not data.
  mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
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
      QMessageBox::warning(this, "Steam import failed", QString::fromStdString(result.error));
      return;
    }
    QMessageBox::information(
        this, "Steam import",
        QString("Added %1 game(s), updated %2.").arg(result.added).arg(result.updated));
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
  cover_cache_.clear();
  delegate_->SetTileSize(TileSize());
  grid_->setGridSize(TileSize());
  ApplyFilter();
}

QPixmap LibraryWindow::CoverFor(const mira_gui::GameSummary& game) {
  const QString key = QString("%1@%2").arg(QString::fromStdString(game.id)).arg(tile_width_);
  auto cached = cover_cache_.constFind(key);
  if (cached != cover_cache_.constEnd()) return *cached;

  const QSize tile = TileSize();
  const QPixmap cover = mira_gui::PlaceholderCover(
      QString::fromStdString(game.name), QString::fromStdString(game.id),
      QSize(tile.width() - 10, tile.height() - 10), devicePixelRatioF());
  cover_cache_.insert(key, cover);
  return cover;
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

void LibraryWindow::RefreshHealth() {
  health_badge_->setText("● checking…");
  health_badge_->setStyleSheet("font-size: 11px; color: #757575;");

  mira_gui::MiradClient::CheckHealthAsync(this, [this](mira_gui::HealthStatus status) {
    SetHealthy(status.reachable, QString::fromStdString(status.detail));
    if (status.reachable) {
      RescanAndRefreshGames();
    } else {
      games_.clear();
      ApplyFilter();
    }
  });
}

void LibraryWindow::RescanAndRefreshGames() {
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
  // A rename changes the cover's initials, so every cached tile for this id
  // is stale — not just the one at the current tile size, or moving the zoom
  // slider afterwards would bring the old initials back.
  const QString prefix = QString::fromStdString(game.id) + "@";
  for (auto it = cover_cache_.begin(); it != cover_cache_.end();) {
    it = it.key().startsWith(prefix) ? cover_cache_.erase(it) : std::next(it);
  }

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
  mira_gui::actions::Launch(this, id, [this, id] {
    running_ids_.insert(id);
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

  mira_gui::GameSummary game;
  if (mira_gui::MiradClient::ParseGameSummary(data, &game)) UpsertGame(game);
}
