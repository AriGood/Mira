#pragma once

#include <QColor>
#include <QString>

#include <functional>

class QWidget;

namespace mira_gui::notify {

// Failed/Warn: desktop notification that stays until dismissed. Notice: one
// that times out; skip it when the result is already on screen. Popups are
// only for questions (Confirm) and content (Info). With no notification
// service running, Failed falls back to a popup and Warn/Notice to an
// in-window card, so nothing is lost.

enum class Level { Info, Success, Warning, Error };

// The wire spelling used by mirad's `notification` event (level field).
// An unknown string reads as Info rather than as an error.
Level LevelFromString(const QString& text);

// The theme color a level reads as. Shared by the fallback card and
// PopupDialog.
QColor AccentFor(Level level);

// --- Notifications -----------------------------------------------------------

// The standard failure. `what` names what failed, as a sentence the user
// could have said themselves; `detail` is mirad's own message, verbatim.
void Failed(QWidget* parent, const QString& what, const QString& detail);

// Same, plus one line of what to do about it.
void FailedWithHint(QWidget* parent, const QString& what, const QString& detail,
                    const QString& hint);

// Same, plus an `action` button that raises the window and runs `activate`.
void FailedWithAction(QWidget* parent, const QString& what, const QString& detail,
                      const QString& hint, const QString& action,
                      std::function<void()> activate);

// A persistent one-liner with no separate detail — mirad's own warnings.
void Warn(QWidget* parent, const QString& text);

// A transient one-liner.
void Notice(QWidget* parent, const QString& text);

// --- Popups ------------------------------------------------------------------

// Content the user asked to see (e.g. a runner kind's config keys) — not a
// notification, the answer to a button.
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

}  // namespace mira_gui::notify
