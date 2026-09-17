#pragma once

#include <QString>

class QWidget;

namespace mira_gui::notify {

// How Mira says things, in one place, so that two screens never disagree
// about what a failure looks like.
//
// Three shapes, and the rule for choosing between them:
//
//  - **Popup** — something you just asked for did not happen, or Mira needs
//    an answer before it can continue. It interrupts, which is correct
//    precisely because the request is still in your head.
//  - **Toast** — something finished on the daemon's schedule rather than
//    yours: a runner download, a metadata fetch, a winetricks verb. Every
//    endpoint that returns 202 (docs/api.md) reports here. By the time one
//    of those finishes you have moved on, and a modal would be an ambush.
//  - **Inline status** — a dialog that owns a long operation reports it in
//    its own status line (see RunnerDialog). It is the only one of the
//    three that can say "still going", so it is the only one that fits a
//    progress report.
//
// The split is not cosmetic: it is the difference between a message the
// user is waiting for and one that arrives behind their back.

enum class Level { Info, Success, Warning, Error };

// --- Popups ----------------------------------------------------------------

// The standard failure popup. `what` names what failed, as a sentence the
// user could have said themselves ("Could not launch Animal Well"), and
// `detail` is mirad's own message, shown verbatim underneath — the daemon
// explains its own refusals better than a rewrite would.
void Failed(QWidget* parent, const QString& what, const QString& detail);

// Same, plus one line of what to do about it. For a failure with an actual
// next step, which is the only kind worth adding a sentence to.
void FailedWithHint(QWidget* parent, const QString& what, const QString& detail,
                    const QString& hint);

void Info(QWidget* parent, const QString& title, const QString& message);

// A question the caller must not proceed without an answer to. `accept`
// labels the affirmative button with the verb about to happen, never "OK":
// a button that says what it does is the only kind that can be read in a
// hurry. `destructive` keeps the focus on Cancel.
bool Confirm(QWidget* parent, const QString& title, const QString& question,
             const QString& accept, bool destructive = false);

// --- Toasts ----------------------------------------------------------------

// A transient card stacked in the bottom-right of `parent`'s window. It
// dismisses itself, and a click dismisses it early.
//
// It attaches to the top-level window, not to `parent`, so a toast raised
// from inside a dialog survives that dialog closing — which is the usual
// case, since the thing being reported started there and finished later.
void Toast(QWidget* parent, Level level, const QString& text);

}  // namespace mira_gui::notify
