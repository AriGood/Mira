#pragma once

#include <QColor>
#include <QString>

#include <functional>

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

// The theme color a level reads as. Shared by ToastCard and PopupDialog.
QColor AccentFor(Level level);

// --- Popups ----------------------------------------------------------------

// The standard failure popup. `what` names what failed, as a sentence the
// user could have said themselves; `detail` is mirad's own message, shown
// verbatim underneath.
void Failed(QWidget* parent, const QString& what, const QString& detail);

// Same, plus one line of what to do about it.
void FailedWithHint(QWidget* parent, const QString& what, const QString& detail,
                    const QString& hint);

// Same, plus a clickable `action` link that runs `activate` — a route to
// wherever actually fixes the problem, not just an explanation of it.
void FailedWithAction(QWidget* parent, const QString& what, const QString& detail,
                      const QString& hint, const QString& action,
                      std::function<void()> activate);

void Info(QWidget* parent, const QString& title, const QString& message);

// A question the caller must not proceed without an answer to. `accept`
// labels the affirmative button with the verb about to happen, never "OK".
// `destructive` keeps the focus on Cancel.
bool Confirm(QWidget* parent, const QString& title, const QString& question,
             const QString& accept, bool destructive = false);

// What a caller leaving a dirty form asked for.
enum class UnsavedAction { Cancel, SaveAndExit, DiscardAndExit };

// The unsaved-edits prompt every Back/Close/Quit path shares. `what` names
// what's unsaved, as a sentence ("This game's edits aren't saved."). No
// separate Cancel button — the popup's own top-right X is that answer.
UnsavedAction ConfirmUnsaved(QWidget* parent, const QString& what);

// --- Toasts ----------------------------------------------------------------

// Always the desktop's own notification service (ui/SystemNotifier), never
// an in-window card — the user has usually moved on to another window by
// the time one of these fires, and only Mira's own window could show a
// card. The one exception: no notification service reachable at all (a bare
// window manager, no daemon), where `Toast` falls back to a card itself so
// the message isn't just lost. Not a preference — there's nothing to set.

// Seconds a toast stays up. Zero (the default) means until dismissed. Also
// the desktop notification's own expire timeout. Clamped to
// kMaxTimeoutSeconds since the value is hand-editable.
constexpr int kMaxTimeoutSeconds = 600;
void SetTimeoutSeconds(int seconds);
int CurrentTimeoutSeconds();

// Sends to the desktop's notification service, or — only when none is
// reachable — a card stacked bottom-right of `parent`'s window. Attaches to
// the top-level window, not `parent`, so it survives a dialog closing.
void Toast(QWidget* parent, Level level, const QString& text);

}  // namespace mira_gui::notify
