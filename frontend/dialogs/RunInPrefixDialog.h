#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include "../client/Types.h"

class QComboBox;
class QLineEdit;
class QPushButton;

namespace mira_gui {

// "Run something inside this game's prefix" — `POST /v1/games/{id}/run`.
//
// The executable box is prefilled from the game's own detected candidates,
// with the installer ones first, since the whole reason to be here is
// usually to run the setup.exe that made the game `needs_install`.
class RunInPrefixDialog : public QDialog {
  Q_OBJECT

public:
  RunInPrefixDialog(std::string game_id, const GameDetail& game, QWidget* parent = nullptr);

private:
  void Run();

  std::string game_id_;
  QComboBox* exe_ = nullptr;
  QLineEdit* args_ = nullptr;
  QPushButton* run_ = nullptr;
};

}  // namespace mira_gui
