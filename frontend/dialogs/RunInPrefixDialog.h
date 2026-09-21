#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include "../client/Types.h"

class QLineEdit;
class QPushButton;

namespace mira_gui {

// "Run something inside this game's prefix" — `POST /v1/games/{id}/run`.
//
// The executable is picked with a file browser rather than prefilled from a
// folder scan: install_path is usually a Wine prefix full of DLLs and
// support files alongside the real executable, and a detected-candidates
// list there tends to be mostly noise.
class RunInPrefixDialog : public QDialog {
  Q_OBJECT

public:
  RunInPrefixDialog(std::string game_id, const GameDetail& game, QWidget* parent = nullptr);

private:
  void Run();

  std::string game_id_;
  std::string install_path_;
  QLineEdit* exe_ = nullptr;
  QLineEdit* args_ = nullptr;
  QPushButton* run_ = nullptr;
};

}  // namespace mira_gui
