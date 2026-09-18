#pragma once

class QMainWindow;

// A single system tray icon for the process, attached to whichever window
// main.cpp treats as primary (the grid, or the table under --classic).
//
// What this buys: the daemon keeps a launched game tracked, and playtime
// keeps accruing, whether or not any Mira window is on screen — closing
// the window is not the same thing as quitting the app. That's the whole
// point of a tray icon; without one there is nowhere for the app to go
// when its one window closes, so it has to actually quit, which is exactly
// what this falls back to when the desktop offers no tray at all.
namespace mira_gui::tray {

// False on a desktop with no tray (QSystemTrayIcon::isSystemTrayAvailable()
// said no, or Attach() was never called). A window's closeEvent checks
// this before deciding to hide instead of close — with no tray, closing
// the window is the only way to leave, so it has to actually leave.
bool Available();

// Creates the tray icon and makes `window` the one it manages: shown or
// hidden together, and the only window whose closeEvent should hide
// instead of close (see IsManaged). A no-op past the first call — one tray
// icon per process, matching one primary window per process (main.cpp
// creates exactly one of LibraryWindow/MainWindow as primary; the other
// view, opened alongside it from a menu, is a secondary window this never
// touches and which keeps closing for real).
void Attach(QMainWindow* window);

// Whether `window` is the one Attach() was given. A secondary window (the
// classic view opened from the grid's Tools menu, or vice versa) answers
// false here even while the primary window's tray icon exists, so closing
// it behaves exactly as it did before there was a tray at all — hiding it
// with nothing left able to bring it back would just lose it.
bool IsManaged(QMainWindow* window);

// True once Quit has actually been chosen — the tray menu's Quit action,
// or Ctrl+Q via RequestQuit() below. A managed window's closeEvent reads
// this to tell "the user closed the window" (hide) from "the user is
// quitting" (let it close for real) apart; the close event alone can't say
// which, since both arrive as the same QCloseEvent.
bool Quitting();

// Sets Quitting() true, then closes every window for real —
// QApplication::closeAllWindows(), so each one still gets its closeEvent
// and LibraryWindow/MainWindow still save their layout in it. The flag is
// set first so that by the time any closeEvent runs, Quitting() already
// answers true. Both Ctrl+Q (Shortcuts.cpp) and the tray menu's Quit
// action call this rather than closeAllWindows() directly, so there is
// exactly one place a real quit begins.
void RequestQuit();

}  // namespace mira_gui::tray
