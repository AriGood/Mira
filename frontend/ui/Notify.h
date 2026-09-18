#pragma once

#include <QString>

class QWidget;

namespace mira_gui::notify {

// How Mira says things, in one place, so two screens never disagree about
// what a failure looks like.
//
// Three shapes:
//  - **Popup** — something you just asked for did not happen, or Mira needs
//    an answer before continuing. Interrupts, correctly.
//  - **Toast** — something finished on the daemon's schedule, not yours (a
//    runner download, a metadata fetch, a winetricks verb). A modal by then
//    would be an ambush.
//  - **Inline status** — a dialog that owns a long operation reports it in
//    its own status line (see RunnerDialog); the only shape that can say
//    "still going".

enum class Level { Info, Success, Warning, Error };

// The wire spelling used by mirad's `notification` event (level field).
// An unknown string reads as Info rather than as an error.
Level LevelFromString(const QString& text);

// --- Popups ----------------------------------------------------------------

// The standard failure popup. `what` names what failed, as a sentence the
// user could have said themselves; `detail` is mirad's own message, shown
// verbatim underneath.
void Failed(QWidget* parent, const QString& what, const QString& detail);

// Same, plus one line of what to do about it.
void FailedWithHint(QWidget* parent, const QString& what, const QString& detail,
                    const QString& hint);

void Info(QWidget* parent, const QString& title, const QString& message);

// A question the caller must not proceed without an answer to. `accept`
// labels the affirmative button with the verb about to happen, never "OK".
// `destructive` keeps the focus on Cancel.
bool Confirm(QWidget* parent, const QString& title, const QString& question,
             const QString& accept, bool destructive = false);

// --- Toasts ----------------------------------------------------------------

// Where a toast is shown. `Auto` sends it to the desktop's notification
// service when Mira's window is not active, and draws the in-window card
// when it is.
enum class Delivery { Auto, System, InApp };

// Read from frontend.toml at startup — see LibraryWindow::LoadPrefs. Falls
// back to the in-window card whenever the system route is unavailable, so
// this is a preference and never a way to lose a message.
void SetDelivery(Delivery delivery);
Delivery CurrentDelivery();

// The frontend.toml spelling of a Delivery, and back. An unknown string
// reads as Auto rather than as an error — the file is hand-editable.
QString DeliveryToString(Delivery delivery);
Delivery DeliveryFromString(const QString& text);

// How long a toast stays up, in seconds. **Zero means until dismissed**,
// and that is the default — these report things that happened while the
// user was elsewhere, and are worth still being there when they look back.
//
// Clamped to kMaxTimeoutSeconds since the value is hand-editable and a
// nonsense one should read as "a long time", not an overflow.
constexpr int kMaxTimeoutSeconds = 600;
void SetTimeoutSeconds(int seconds);
int CurrentTimeoutSeconds();

// A transient card stacked in the bottom-right of `parent`'s window. A click
// dismisses it early.
//
// Attaches to the top-level window, not to `parent`, so a toast raised from
// inside a dialog survives that dialog closing.
void Toast(QWidget* parent, Level level, const QString& text);

}  // namespace mira_gui::notify
