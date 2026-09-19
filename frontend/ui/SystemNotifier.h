#pragma once

#include <QString>

#include "Notify.h"

namespace mira_gui::notify {

// The desktop's own notification service, org.freedesktop.Notifications —
// where every toast goes (ui/Notify.h). A system notification survives the
// window being minimised, lands in the desktop's notification history, and
// obeys Do Not Disturb — an in-window card does none of that. `Toast` falls
// back to a card only when Available() is false.
namespace system_notifier {

// True if Mira's own mira.desktop is installed where the desktop can find
// it — $XDG_DATA_HOME/applications, then each of $XDG_DATA_DIRS.
//
// Worth checking rather than asserting: running out of a build tree,
// nothing is installed, and claiming the app id anyway makes
// xdg-desktop-portal log a warning on every start that the developer can't
// act on.
bool DesktopEntryInstalled();

// True if a notification service answered. Queried once and remembered —
// the service is owned by the session's shell and does not come and go.
bool Available();

// Sends one. False if it could not be delivered, which is the caller's cue
// to draw the in-window card instead.
bool Send(Level level, const QString& text);

}  // namespace system_notifier
}  // namespace mira_gui::notify
