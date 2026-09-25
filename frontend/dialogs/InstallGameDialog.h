#pragma once

#include <QDialog>
#include <QString>

#include <string>

class QLabel;
class QPushButton;

namespace mira_gui {

// Runs a game's installer through mirad (`POST /v1/games/{id}/install`):
// shows the detected installer, lets the user pick a different file, and
// asks whether to run it quietly (supported formats only) or with its window. Closes once mirad has
// started it; progress shows on the game's tile.
class InstallGameDialog : public QDialog {
  Q_OBJECT

public:
  InstallGameDialog(std::string game_id, const std::string& install_path, const QString& name,
                    QWidget* parent = nullptr);

private:
  void LoadInfo(const std::string& path);
  void Install(bool interactive);

  std::string game_id_;
  std::string install_path_;
  std::string installer_;  // empty: the game's own
  QLabel* info_ = nullptr;
  QLabel* error_ = nullptr;
  QPushButton* shown_ = nullptr;
  QPushButton* quiet_ = nullptr;
};

}  // namespace mira_gui
