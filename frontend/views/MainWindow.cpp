#include "MainWindow.h"

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include "../client/MiradClient.h"
#include "../dialogs/GameDetailDialog.h"
#include "../dialogs/SettingsDialog.h"
#include "../ui/GameActions.h"
#include "../ui/GamePresentation.h"
#include "../ui/Shortcuts.h"

namespace {

// A table cell that sorts on a stashed numeric value instead of its display
// text — needed for Confidence ("70%" vs "100%" sorts wrong as text),
// Last Played (a formatted date), and Playtime ("1h 5m" vs "45m").
class NumericTableWidgetItem : public QTableWidgetItem {
public:
  NumericTableWidgetItem(const QString& text, double sort_value)
      : QTableWidgetItem(text), sort_value_(sort_value) {}

  bool operator<(const QTableWidgetItem& other) const override {
    if (const auto* numeric = dynamic_cast<const NumericTableWidgetItem*>(&other)) {
      return sort_value_ < numeric->sort_value_;
    }
    return QTableWidgetItem::operator<(other);
  }

private:
  double sort_value_;
};

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("Mira");
  resize(1000, 650);

  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(6);

  auto* header_row = new QHBoxLayout();
  auto* title_label = new QLabel("Mira", central);
  title_label->setStyleSheet("font-size: 18px; font-weight: 600;");

  health_badge_ = new QLabel(central);
  health_badge_->setStyleSheet("font-size: 11px; color: #757575;");
  health_badge_->setText("● checking…");

  status_filter_ = new QComboBox(central);
  status_filter_->addItem("All statuses", "");
  status_filter_->addItem("Ready", "ready");
  status_filter_->addItem("Setting up", "setting_up");
  status_filter_->addItem("Needs install", "needs_install");
  status_filter_->addItem("Broken", "broken");
  status_filter_->addItem("Missing", "missing");
  connect(status_filter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] { RefreshGames(); });

  settings_button_ = new QPushButton("Settings", central);
  settings_button_->setMaximumWidth(72);
  connect(settings_button_, &QPushButton::clicked, this, &MainWindow::OpenSettings);

  refresh_button_ = new QPushButton("Refresh", central);
  refresh_button_->setEnabled(false);
  refresh_button_->setMaximumWidth(72);
  connect(refresh_button_, &QPushButton::clicked, this, &MainWindow::RefreshHealth);

  header_row->addWidget(title_label);
  header_row->addStretch(1);
  header_row->addWidget(health_badge_);
  header_row->addWidget(status_filter_);
  header_row->addWidget(settings_button_);
  header_row->addWidget(refresh_button_);
  layout->addLayout(header_row);

  games_table_ = new QTableWidget(0, 8, central);
  games_table_->setHorizontalHeaderLabels(
      {"Name", "Status", "Platform", "Runner", "Confidence", "Last Played", "Playtime", ""});
  games_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  games_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  games_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  games_table_->setAlternatingRowColors(true);
  games_table_->setSortingEnabled(true);
  games_table_->verticalHeader()->setVisible(false);
  games_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int column = 1; column <= 7; ++column) {
    games_table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
  }
  games_table_->setShowGrid(false);
  connect(games_table_, &QTableWidget::cellDoubleClicked, this, &MainWindow::OpenGameDetail);
  layout->addWidget(games_table_, /*stretch=*/1);

  connection_footer_ = new QLabel(central);
  connection_footer_->setStyleSheet("font-size: 10px; color: #9e9e9e;");
  connection_footer_->setText(
      QString("Connected via %1").arg(QString::fromStdString(mira_gui::MiradClient::ResolveSocketPath())));
  layout->addWidget(connection_footer_);

  setCentralWidget(central);

  BuildShortcuts();

  RefreshHealth();

  event_stream_.Start(this,
                       [this](std::string type, std::string data) { HandleGameEvent(type, data); });
}

void MainWindow::BuildShortcuts() {
  // The classic view has no menu bar, so these keys are only reachable
  // through the reference dialog F1 opens — which is why it lists them.
  mira_gui::shortcuts::Install(this, {
                                        {"F5, Ctrl+R", "Refresh the library"},
                                        {"Enter", "Details & settings for the selected row"},
                                        {"Delete", "Remove the selected game"},
                                        {"Ctrl+,", "Settings"},
                                    });

  auto window_action = [this](std::initializer_list<QKeySequence> keys, auto slot) {
    auto* action = new QAction(this);
    action->setShortcuts(QList<QKeySequence>(keys));
    connect(action, &QAction::triggered, this, slot);
    addAction(action);
  };

  window_action({QKeySequence(QKeySequence::Refresh), QKeySequence(Qt::CTRL | Qt::Key_R)}, [this] {
    // Through the button so its disabled-while-checking state still holds:
    // a key that could fire a second health check mid-flight would be the
    // one way to get two of them running at once.
    if (refresh_button_->isEnabled()) RefreshHealth();
  });
  window_action({QKeySequence(Qt::CTRL | Qt::Key_Comma)}, [this] { OpenSettings(); });

  // Scoped to the table, so Enter and Delete keep their normal meaning in
  // the status filter's popup and anywhere else focus can land.
  auto table_action = [this](std::initializer_list<QKeySequence> keys, auto slot) {
    auto* action = new QAction(games_table_);
    action->setShortcuts(QList<QKeySequence>(keys));
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(action, &QAction::triggered, this, slot);
    games_table_->addAction(action);
  };

  table_action({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)}, [this] {
    const int row = games_table_->currentRow();
    if (row >= 0) OpenGameDetail(row, 0);
  });

  table_action({QKeySequence(Qt::Key_Delete)}, [this] {
    const QTableWidgetItem* item = games_table_->item(games_table_->currentRow(), 0);
    if (item == nullptr) return;
    DeleteGame(item->data(Qt::UserRole).toString().toStdString(), item->text());
  });
}

void MainWindow::SetHealthy(bool healthy, const QString& tooltip) {
  health_badge_->setToolTip(tooltip);
  if (healthy) {
    health_badge_->setText("● Online");
    health_badge_->setStyleSheet("font-size: 11px; color: #2e7d32; font-weight: 600;");
  } else {
    health_badge_->setText("● Offline");
    health_badge_->setStyleSheet("font-size: 11px; color: #c62828; font-weight: 600;");
  }
}

void MainWindow::RefreshHealth() {
  health_badge_->setText("● checking…");
  health_badge_->setStyleSheet("font-size: 11px; color: #757575;");
  refresh_button_->setEnabled(false);

  mira_gui::MiradClient::CheckHealthAsync(this, [this](mira_gui::HealthStatus status) {
    refresh_button_->setEnabled(true);
    SetHealthy(status.reachable, QString::fromStdString(status.detail));
    if (status.reachable) {
      RescanAndRefreshGames();
    } else {
      games_table_->setRowCount(0);
    }
  });
}

void MainWindow::RescanAndRefreshGames() {
  if (!loaded_) {
    // First load: a scan only reports changes, not what already existed.
    mira_gui::MiradClient::ScanLibraryAsync(this, [this](mira_gui::ScanResult) { RefreshGames(); });
    return;
  }
  // Kept in sync since by game.added/.updated/.removed events.
  mira_gui::MiradClient::ScanLibraryAsync(this, [](mira_gui::ScanResult) {});
}

std::string MainWindow::CurrentStatusFilter() const {
  return status_filter_->currentData().toString().toStdString();
}

void MainWindow::RefreshGames() {
  mira_gui::MiradClient::ListGamesAsync(
      this,
      [this](mira_gui::GamesResult result) {
        if (!result.ok) {
          health_badge_->setToolTip(
              QString("mirad is reachable, but GET /v1/games failed: %1")
                  .arg(QString::fromStdString(result.error)));
          games_table_->setRowCount(0);
          return;
        }

        // Disabled for the bulk repopulate below: with sorting live, each
        // setItem() call would re-sort the table mid-loop, so row indices
        // would stop matching what PopulateRow was just given.
        games_table_->setSortingEnabled(false);
        games_table_->setRowCount(static_cast<int>(result.games.size()));
        for (int row = 0; row < static_cast<int>(result.games.size()); ++row) {
          PopulateRow(row, result.games[row]);
        }
        games_table_->setSortingEnabled(true);
        loaded_ = true;
      },
      CurrentStatusFilter());
}

int MainWindow::FindRow(const std::string& id) const {
  const QString target = QString::fromStdString(id);
  for (int row = 0; row < games_table_->rowCount(); ++row) {
    // A freshly inserted row has no items until PopulateRow fills it, so
    // this can legitimately be null — dereferencing it unconditionally
    // turns any such moment into a crash.
    const QTableWidgetItem* item = games_table_->item(row, 0);
    if (item != nullptr && item->data(Qt::UserRole).toString() == target) return row;
  }
  return -1;
}

void MainWindow::PopulateRow(int row, const mira_gui::GameSummary& game) {
  auto* name_item = new QTableWidgetItem(QString::fromStdString(game.name));
  name_item->setData(Qt::UserRole, QString::fromStdString(game.id));

  auto* status_item = new QTableWidgetItem(QString::fromStdString(game.status));
  status_item->setForeground(mira_gui::StatusColor(game.status));
  if (!game.last_error.empty()) status_item->setToolTip(QString::fromStdString(game.last_error));

  auto* platform_item = new QTableWidgetItem(QString::fromStdString(game.platform));

  auto* runner_item = new QTableWidgetItem(
      game.runner_ref.empty() ? "Auto" : QString::fromStdString(game.runner_ref));

  auto* confidence_item = new NumericTableWidgetItem(
      mira_gui::ConfidenceText(game.reviewed, game.confidence), game.confidence);
  confidence_item->setForeground(mira_gui::ConfidenceColor(game.reviewed, game.confidence));

  auto* last_played_item = new NumericTableWidgetItem(
      mira_gui::FormatLastPlayed(game.last_played_at), static_cast<double>(game.last_played_at.value_or(-1)));

  auto* playtime_item =
      new NumericTableWidgetItem(mira_gui::FormatPlaytime(game.play_seconds), static_cast<double>(game.play_seconds));

  games_table_->setItem(row, 0, name_item);
  games_table_->setItem(row, 1, status_item);
  games_table_->setItem(row, 2, platform_item);
  games_table_->setItem(row, 3, runner_item);
  games_table_->setItem(row, 4, confidence_item);
  games_table_->setItem(row, 5, last_played_item);
  games_table_->setItem(row, 6, playtime_item);

  auto* actions_widget = new QWidget(games_table_);
  auto* actions_layout = new QHBoxLayout(actions_widget);
  actions_layout->setContentsMargins(0, 0, 0, 0);
  actions_layout->setSpacing(4);

  const std::string id = game.id;
  const QString name = QString::fromStdString(game.name);
  const bool running = running_ids_.contains(id);

  auto* launch_button = new QPushButton(running ? "Stop" : "Launch", games_table_);
  // Mirrors POST /v1/games/{id}/launch's own guard (docs/api.md: 409
  // needs_install/not_ready) so a doomed request never leaves this process
  // — the button is simply disabled instead of round-tripping to find out.
  const bool can_launch = game.status == "ready";
  launch_button->setEnabled(running || can_launch);
  if (!running && !can_launch) {
    launch_button->setToolTip(
        QString("Not launchable while %1").arg(QString::fromStdString(game.status)));
  }
  connect(launch_button, &QPushButton::clicked, this, [this, id, running] {
    if (running) {
      StopGame(id);
    } else {
      LaunchGame(id);
    }
  });
  actions_layout->addWidget(launch_button);

  auto* delete_button = new QPushButton("Delete", games_table_);
  connect(delete_button, &QPushButton::clicked, this, [this, id, name] { DeleteGame(id, name); });
  actions_layout->addWidget(delete_button);

  games_table_->setCellWidget(row, 7, actions_widget);
}

void MainWindow::UpsertRow(const mira_gui::GameSummary& game) {
  const std::string filter = CurrentStatusFilter();
  int row = FindRow(game.id);

  if (!filter.empty() && filter != game.status) {
    // No longer matches the active filter (or never did) — drop it from
    // view rather than showing a row that contradicts the filter, but
    // without touching the daemon's own record.
    if (row >= 0) games_table_->removeRow(row);
    return;
  }

  if (row < 0) {
    row = games_table_->rowCount();
    games_table_->insertRow(row);
  }

  // Sorting off for the same reason RefreshGames turns it off: Qt re-sorts
  // on any change to the sort column, so PopulateRow's very first setItem()
  // can move this row, and every column after it — plus the actions cell —
  // would then be written into whatever row slid into this index. That hands
  // one game another's status and playtime, and puts its Delete button on
  // the wrong row, which is a good deal worse than a cosmetic glitch.
  // Re-enabling sorts again, so a rename still lands in its new position.
  const bool sorting = games_table_->isSortingEnabled();
  games_table_->setSortingEnabled(false);
  PopulateRow(row, game);
  games_table_->setSortingEnabled(sorting);
}

void MainWindow::RemoveRow(const std::string& id) {
  const int row = FindRow(id);
  if (row >= 0) games_table_->removeRow(row);
}

void MainWindow::HandleGameEvent(const std::string& type, const std::string& data) {
  if (type == "game.removed") {
    const std::string id = mira_gui::MiradClient::ParseRemovedId(data);
    if (!id.empty()) RemoveRow(id);
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
      UpsertRow(game);
    } else {
      RefreshGames();
    }
    return;
  }

  if (type == "game.launched") {
    // The untracked counterpart to game.state: mirad handed this one to
    // Steam. Clearing rather than ignoring, because the launch may have come
    // from somewhere else (the CLI, another window) that did mark it.
    const std::string id = mira_gui::MiradClient::ParseRemovedId(data);
    if (!id.empty()) {
      running_ids_.erase(id);
      RefreshGames();
    }
    return;
  }

  // Explicitly the two event types that carry a game record, rather than
  // "anything left over". mirad publishes runners.download.* and tricks.*
  // on the same stream, and treating an unrecognised payload as a game was
  // how a runner download added a blank tile to the library — and how a
  // tricks event would have blanked a real one, since it carries an id.
  if (type != "game.added" && type != "game.updated") return;

  mira_gui::GameSummary game;
  if (mira_gui::MiradClient::ParseGameSummary(data, &game)) UpsertRow(game);
}

void MainWindow::DeleteGame(const std::string& id, const QString& name) {
  mira_gui::actions::Delete(this, id, name, [this] { RefreshGames(); });
}

void MainWindow::LaunchGame(const std::string& id) {
  mira_gui::actions::Launch(this, id, [this, id](bool tracked) {
    // Not waiting for the game.state "running" event to confirm this: it's
    // on its way regardless, so marking it now avoids a window where a
    // second click could fire another launch before the event arrives.
    //
    // Unless it isn't on its way. A Steam-launched game is never tracked, so
    // marking it running here would leave it running forever — there is no
    // exit event to clear it.
    if (tracked) running_ids_.insert(id);
    RefreshGames();
  });
}

void MainWindow::StopGame(const std::string& id) {
  // Left in running_ids_ either way — Stop only sends SIGTERM and returns;
  // the real state change arrives later as game.state "exited"/"crashed"
  // (docs/api.md), same as with a game that quits on its own.
  mira_gui::actions::Stop(this, id);
}

void MainWindow::OpenGameDetail(int row, int /*column*/) {
  auto* item = games_table_->item(row, 0);
  if (!item) return;
  const std::string id = item->data(Qt::UserRole).toString().toStdString();
  GameDetailDialog dialog(id, this);
  dialog.exec();
}

void MainWindow::OpenSettings() {
  SettingsDialog dialog(this);
  dialog.exec();
}
