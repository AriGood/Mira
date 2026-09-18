#include "Tray.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMainWindow>
#include <QMenu>
#include <QSystemTrayIcon>

namespace mira_gui::tray {
namespace {

QSystemTrayIcon* g_icon = nullptr;
QMainWindow* g_window = nullptr;
bool g_quitting = false;

void Restore() {
  if (g_window == nullptr) return;
  g_window->show();
  g_window->raise();
  g_window->activateWindow();
}

}  // namespace

bool Available() { return g_icon != nullptr; }

bool IsManaged(QMainWindow* window) { return window != nullptr && window == g_window; }

bool Quitting() { return g_quitting; }

void RequestQuit() {
  g_quitting = true;
  QApplication::closeAllWindows();
  // Needed because Attach() turns quitOnLastWindowClosed off (see there):
  // closeAllWindows() alone would leave every window closed and the event
  // loop still running with nothing left to stop it.
  QApplication::quit();
}

void Attach(QMainWindow* window) {
  if (g_icon != nullptr || window == nullptr) return;  // one per process
  if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

  g_window = window;
  // Otherwise Qt quits the app the moment the window's visible count hits
  // zero — which hide-to-tray does on purpose. That's the whole feature a
  // tray icon is supposed to provide, so this is the one line that makes
  // the rest of this file mean anything; RequestQuit() below is what
  // actually ends the process now that this can't.
  QApplication::setQuitOnLastWindowClosed(false);

  // The same icon main.cpp built for the window/taskbar — see the comment
  // there on why it's a compiled-in resource rather than QIcon::fromTheme.
  g_icon = new QSystemTrayIcon(QApplication::windowIcon(), window);
  g_icon->setToolTip("Mira");

  auto* menu = new QMenu(window);
  QAction* toggle = menu->addAction("Show/Hide Mira");
  QObject::connect(toggle, &QAction::triggered, toggle, [] {
    if (g_window == nullptr) return;
    if (g_window->isVisible()) {
      g_window->hide();
    } else {
      Restore();
    }
  });
  menu->addSeparator();
  menu->addAction("&Quit", &RequestQuit);
  g_icon->setContextMenu(menu);

  // Relabelled to whichever half actually applies each time the menu
  // opens — a single "Show/Hide" reads as unfinished, and there's only
  // ever one of the two that does anything.
  QObject::connect(menu, &QMenu::aboutToShow, toggle, [toggle] {
    toggle->setText(g_window != nullptr && g_window->isVisible() ? "Hide Mira" : "Show Mira");
  });

  QObject::connect(g_icon, &QSystemTrayIcon::activated, window,
                   [](QSystemTrayIcon::ActivationReason reason) {
                     // Trigger is a left click (X11: single, most desktops);
                     // DoubleClick covers the platforms that reserve
                     // Trigger for something else. Both mean the same
                     // thing here: bring the window to the front, since a
                     // hidden window has no other click target to restore
                     // it from.
                     if (reason == QSystemTrayIcon::Trigger ||
                         reason == QSystemTrayIcon::DoubleClick) {
                       Restore();
                     }
                   });

  g_icon->show();
}

}  // namespace mira_gui::tray
