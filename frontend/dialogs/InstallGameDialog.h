#pragma once

#include <QDialog>
#include <QString>

#include <string>

class QCheckBox;
class QLabel;
class QPushButton;

namespace mira_gui {

// Runs a game's installer through mirad (`POST /v1/games/{id}/install`):
// shows what the installer is and whether Mira can run it silently, and
// lets the user pick a different installer file. Closes once mirad has
// started it; progress shows on the game's tile.
class InstallGameDialog : public QDialog {
  Q_OBJECT

public:
  InstallGameDialog(std::string game_id, const std::string& install_path, const QString& name,
                    QWidget* parent = nullptr);

private:
  void LoadInfo(const std::string& path);
  void Install();

  std::string game_id_;
  std::string install_path_;
  std::string installer_;  // empty: the game's own
  QLabel* info_ = nullptr;
  QCheckBox* interactive_ = nullptr;
  QLabel* error_ = nullptr;
  QPushButton* install_ = nullptr;
};

}  // namespace mira_gui
