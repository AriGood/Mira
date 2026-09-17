#pragma once

#include <QString>

#include "Notify.h"

namespace mira_gui::notify {

// The desktop's own notification service, org.freedesktop.Notifications —
// Plasma's on KDE, and the same interface everywhere else that has one.
//
// Worth reaching for rather than always drawing our own card: a system
// notification survives the window being minimised or on another desktop,
// lands in Plasma's notification history, and obeys Do Not Disturb. An
// in-window toast does none of that, and is invisible exactly when a
// background job finishing is most worth knowing about.
//
// The two are not interchangeable in the other direction either, which is
// why Delivery::Auto exists: a system popup for something that just
// happened in the window you are looking at is noise the desktop then
// keeps a record of.
namespace system_notifier {

// True if a notification service answered. Queried once and remembered —
// the service is owned by the session's shell and does not come and go.
bool Available();

// Sends one. False if it could not be delivered, which is the caller's cue
// to draw the in-window card instead.
bool Send(Level level, const QString& text);

}  // namespace system_notifier
}  // namespace mira_gui::notify
