#pragma once

#include <QList>
#include <QString>

class QAction;
class QMainWindow;
class QWidget;

namespace mira_gui::shortcuts {

// One row of the keyboard reference dialog.
struct Entry {
  QString keys;
  QString description;
};

// The actions every top-level window carries, whether or not it has a menu
// bar to put them in.
struct Common {
  QAction* quit = nullptr;
  QAction* close_window = nullptr;
  QAction* reference = nullptr;
};

// Creates the common actions and adds them to `window` itself.
//
// Adding them to the window is what makes the keys live: a QAction's
// shortcut only fires while the action belongs to a widget in the focus
// chain, and the classic view has no menu bar to hang one on. A window that
// does have one can still put these in a menu afterwards — the same QAction
// in two places is the normal Qt arrangement, and it is what makes the menu
// display the key that triggers it.
//
// `window_specific` is the rest of this window's keys, listed in the
// reference dialog below the common ones.
Common Install(QMainWindow* window, const QList<Entry>& window_specific = {});

// Every key this window responds to, in one read-only list. Discoverability
// is the whole point: a shortcut nothing ever names is a shortcut only its
// author uses.
void ShowReference(QWidget* parent, const QList<Entry>& window_specific);

}  // namespace mira_gui::shortcuts
