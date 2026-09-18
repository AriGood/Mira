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
// There is a difference between forgetting a game and deleting its files.
// The default is still the safe option (just remove the record), and there
// is an explicit individually-named tick for destructive extras. An empty path
// disables the checkbox rather than hiding it.
DeleteChoice AskDeleteGame(QWidget* parent, const QString& name, const QString& install_path,
                           const QString& data_dir);

}  // namespace mira_gui
