#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QStringList>

#include "ui/SystemNotifier.h"
#include "ui/Tray.h"
#include "views/LibraryWindow.h"
#include "views/MainWindow.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("Mira");
  QApplication::setOrganizationName("mira");
  // Matches packaging/mira.desktop, which is how a Wayland compositor and
  // the notification service both work out which application this is — it
  // is what puts Mira's own name and icon on a desktop notification rather
  // than a generic one.
  //
  // Only when that file is actually installed: claiming an app id nothing
  // can resolve makes xdg-desktop-portal log "Could not register app ID:
  // App info not found for 'mira'" on every start, which is exactly what
  // running from a build tree does.
  if (mira_gui::notify::system_notifier::DesktopEntryInstalled()) {
    QGuiApplication::setDesktopFileName("mira");
  }

  QIcon icon;
  for (int size : {16, 32, 48, 64, 128, 256}) {
    icon.addFile(QString(":/icons/%1x%1/apps/mira.png").arg(size), QSize(size, size));
  }
  QApplication::setWindowIcon(icon);

  // LibraryWindow (the cover grid) is the primary UI; MainWindow (the table)
  // is kept as a working fallback rather than deleted — it shows every field
  // at once, which is what you want when auditing a fresh scan. `--classic`
  // starts straight in it; the grid's View menu opens it alongside.
  // Deliberately not QCommandLineParser: one flag doesn't justify it, and
  // parsing it this way leaves Qt's own arguments (-style, -platform) alone.
  const bool classic = QApplication::arguments().contains("--classic");

  QMainWindow* window = classic ? static_cast<QMainWindow*>(new MainWindow())
                                : static_cast<QMainWindow*>(new LibraryWindow());
  window->setAttribute(Qt::WA_DeleteOnClose);
  // A no-op on a desktop with no tray (Tray.cpp) — window->close() then
  // means exactly what it always did.
  mira_gui::tray::Attach(window);
  window->show();
  return QApplication::exec();
}
