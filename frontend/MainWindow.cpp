#include "MainWindow.h"

#include "GameColors.h"
#include "GameDetailDialog.h"
#include "MiradClient.h"
#include "SettingsDialog.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

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
  header_row->addWidget(settings_button_);
  header_row->addWidget(refresh_button_);
  layout->addLayout(header_row);

  games_table_ = new QTableWidget(0, 5, central);
  games_table_->setHorizontalHeaderLabels({"Name", "Status", "Platform", "Confidence", ""});
  games_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  games_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  games_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  games_table_->setAlternatingRowColors(true);
  games_table_->verticalHeader()->setVisible(false);
  games_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  games_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  games_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  games_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  games_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  games_table_->setShowGrid(false);
  connect(games_table_, &QTableWidget::cellDoubleClicked, this, &MainWindow::OpenGameDetail);
  layout->addWidget(games_table_, /*stretch=*/1);

  setCentralWidget(central);

  RefreshHealth();

  event_stream_.Start(this,
                       [this](std::string type, std::string data) { HandleGameEvent(type, data); });
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
  mira_gui::MiradClient::ScanLibraryAsync(this, [this](mira_gui::ScanResult) { RefreshGames(); });
}

void MainWindow::RefreshGames() {
  mira_gui::MiradClient::ListGamesAsync(this, [this](mira_gui::GamesResult result) {
    if (!result.ok) {
      health_badge_->setToolTip(
          QString("mirad is reachable, but GET /v1/games failed: %1")
              .arg(QString::fromStdString(result.error)));
      games_table_->setRowCount(0);
      return;
    }

    games_table_->setRowCount(static_cast<int>(result.games.size()));
    for (int row = 0; row < static_cast<int>(result.games.size()); ++row) {
      PopulateRow(row, result.games[row]);
    }
  });
}

int MainWindow::FindRow(const std::string& id) const {
  const QString target = QString::fromStdString(id);
  for (int row = 0; row < games_table_->rowCount(); ++row) {
    if (games_table_->item(row, 0)->data(Qt::UserRole).toString() == target) return row;
  }
  return -1;
}

void MainWindow::PopulateRow(int row, const mira_gui::GameSummary& game) {
  auto* name_item = new QTableWidgetItem(QString::fromStdString(game.name));
  name_item->setData(Qt::UserRole, QString::fromStdString(game.id));
  auto* status_item = new QTableWidgetItem(QString::fromStdString(game.status));
  status_item->setForeground(mira_gui::StatusColor(game.status));
  auto* platform_item = new QTableWidgetItem(QString::fromStdString(game.platform));
  auto* review_item =
      new QTableWidgetItem(mira_gui::ConfidenceText(game.reviewed, game.confidence));
  review_item->setForeground(mira_gui::ConfidenceColor(game.reviewed, game.confidence));

  games_table_->setItem(row, 0, name_item);
  games_table_->setItem(row, 1, status_item);
  games_table_->setItem(row, 2, platform_item);
  games_table_->setItem(row, 3, review_item);

  auto* delete_button = new QPushButton("Delete", games_table_);
  const std::string id = game.id;
  const QString name = QString::fromStdString(game.name);
  connect(delete_button, &QPushButton::clicked, this, [this, id, name] { DeleteGame(id, name); });
  games_table_->setCellWidget(row, 4, delete_button);
}

void MainWindow::UpsertRow(const mira_gui::GameSummary& game) {
  int row = FindRow(game.id);
  if (row < 0) {
    row = games_table_->rowCount();
    games_table_->insertRow(row);
  }
  PopulateRow(row, game);
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

  mira_gui::GameSummary game;
  if (mira_gui::MiradClient::ParseGameSummary(data, &game)) UpsertRow(game);
}

void MainWindow::DeleteGame(const std::string& id, const QString& name) {
  const auto choice = QMessageBox::question(
      this, "Remove game",
      QString("Remove \"%1\" from the library? This does not touch its files on disk.").arg(name),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (choice != QMessageBox::Yes) return;

  mira_gui::MiradClient::DeleteGameAsync(this, id, [this, name](mira_gui::DeleteResult result) {
    if (!result.ok) {
      QMessageBox::warning(this, "Remove failed",
                            QString("Failed to remove \"%1\": %2")
                                .arg(name, QString::fromStdString(result.error)));
      return;
    }
    RefreshGames();
  });
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
