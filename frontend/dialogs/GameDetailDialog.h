#pragma once

#include <QDialog>

#include <string>

namespace mira_gui {
class ArtworkStore;
class GameEditForm;
}

class QPushButton;

// A thin QDialog shell around ui/GameEditForm — the modal route to editing
// a game (MainWindow's classic view, or LibraryWindow's fallback when
// game_settings_in_sidebar is off). The embedded route is LibraryWindow
// swapping GameEditForm in full-width instead; both share the one form
// implementation.
class GameDetailDialog : public QDialog {
  Q_OBJECT

public:
  // `artwork`, when given, is the same store the caller's own cover grid
  // uses -- MainWindow's classic table view has none, and the form just
  // shows placeholders in that case.
  GameDetailDialog(std::string id, QWidget* parent = nullptr, mira_gui::ArtworkStore* artwork = nullptr);

  // Warns before discarding unsaved changes — covers Cancel, Esc, and the
  // titlebar close button, which all route through reject().
  void reject() override;

private:
  mira_gui::GameEditForm* form_ = nullptr;
  QPushButton* save_button_ = nullptr;
};
