#pragma once

#include <QDialog>

#include <string>

namespace mira_gui {
class GameEditForm;
}

class QPushButton;

// A thin QDialog shell around ui/GameEditForm — the modal route to editing
// a game (MainWindow's classic view, or the grid's fallback when
// game_settings_in_sidebar is off). The embedded route is
// GameDetailsPanel swapping GameEditForm into the sidebar directly; both
// share the one form implementation.
class GameDetailDialog : public QDialog {
  Q_OBJECT

public:
  GameDetailDialog(std::string id, QWidget* parent = nullptr);

private:
  mira_gui::GameEditForm* form_ = nullptr;
  QPushButton* save_button_ = nullptr;
};
