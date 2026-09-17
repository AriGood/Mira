#pragma once

#include <QMainWindow>
#include <QString>

#include <string>

#include "MiradClient.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

// The frontend's entry point window. Proves mira-gui can reach mirad over
// its REST API (docs/api.md) without linking mira_core, and shows the game
// library it returns.
class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget* parent = nullptr);

private:
  void RefreshHealth();
  void RescanAndRefreshGames();
  void RefreshGames();
  void DeleteGame(const std::string& id, const QString& name);
  void OpenGameDetail(int row, int column);
  void OpenSettings();
  void SetHealthy(bool healthy, const QString& tooltip);

  // Applies one event from event_stream_ directly to the table (docs/api.md:
  // game.added/game.updated carry the full record, game.removed just the
  // id), instead of re-fetching the whole list on every change.
  void HandleGameEvent(const std::string& type, const std::string& data);
  int FindRow(const std::string& id) const;
  void PopulateRow(int row, const mira_gui::GameSummary& game);
  void UpsertRow(const mira_gui::GameSummary& game);
  void RemoveRow(const std::string& id);
  std::string CurrentStatusFilter() const;

  QLabel* health_badge_;
  QComboBox* status_filter_;
  QPushButton* settings_button_;
  QPushButton* refresh_button_;
  QTableWidget* games_table_;
  QLabel* connection_footer_;
  mira_gui::EventStream event_stream_;
};
