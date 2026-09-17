#pragma once

#include <QString>

class QWidget;

namespace mira_gui {

struct DeleteChoice {
  bool confirmed = false;
  bool delete_files = false;   // DELETE /v1/games/{id}?delete_files=true
  bool delete_prefix = false;  // ...&delete_prefix=true
};

// The "remove game" confirmation, shared by both library views.
//
// Forgetting a game and deleting its files are separate decisions in
// docs/api.md, and they stay separate here: the default is still the safe one
// (remove the record, touch nothing on disk) and each destructive extra is an
// explicit, individually-named tick. `install_path` and `data_dir` are shown
// verbatim next to their checkbox — a prompt offering to delete a directory
// without naming it is not a prompt anyone can answer responsibly. An empty
// path disables its checkbox rather than hiding it, so "this game has no
// prefix" reads differently from "Mira won't offer to delete it".
DeleteChoice AskDeleteGame(QWidget* parent, const QString& name, const QString& install_path,
                           const QString& data_dir);

}  // namespace mira_gui
