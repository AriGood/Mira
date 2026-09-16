#pragma once

#include <QMainWindow>
#include <QString>

#include <string>

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
  void SetHealthy(bool healthy, const QString& tooltip);

  QLabel* health_badge_;
  QPushButton* refresh_button_;
  QTableWidget* games_table_;
};
