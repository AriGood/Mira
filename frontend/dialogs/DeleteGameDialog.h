#pragma once

#include <QString>

class QWidget;

namespace mira_gui {

struct DeleteChoice {
  bool confirmed = false;
  bool delete_files = false;     // DELETE /v1/games/{id}?delete_files=true
  bool delete_prefix = false;    // ...&delete_prefix=true
  bool delete_metadata = false;  // ...&delete_metadata=true
};

// The "remove game" confirmation, shared by both library views.
//
// There is a difference between forgetting a game and deleting its files.
// The default is still the safe option (just remove the record), and there
// is an explicit individually-named tick for destructive extras. An empty path
// disables the checkbox rather than hiding it. A "desktop-entry" source game
// never had its own files/prefix to begin with — Mira only ever linked to
// them — so those two checkboxes are disabled regardless of path.
DeleteChoice AskDeleteGame(QWidget* parent, const QString& name, const QString& install_path,
                           const QString& data_dir, const QString& source);

// Same choice, for a batch of games at once. There's no single path to show,
// so this warns in words instead: files/prefix deletion is skipped
// per-game for a desktop-entry-sourced one, same as the single-game dialog.
DeleteChoice AskDeleteGames(QWidget* parent, int count);

}  // namespace mira_gui
