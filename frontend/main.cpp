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
  // Matches packaging/mira.desktop, which is how the compositor and the
  // notification service work out which application this is. Only when
  // that file is actually installed — claiming an unresolvable app id
  // makes xdg-desktop-portal log a warning on every start.
  if (mira_gui::notify::system_notifier::DesktopEntryInstalled()) {
    QGuiApplication::setDesktopFileName("mira");
  }

  QIcon icon;
  for (int size : {16, 32, 48, 64, 128, 256}) {
    icon.addFile(QString(":/icons/%1x%1/apps/mira.png").arg(size), QSize(size, size));
  }
  QApplication::setWindowIcon(icon);

  // LibraryWindow (the cover grid) is primary; MainWindow (the table) is
  // kept as a fallback for auditing a fresh scan. `--classic` starts
  // straight in it. Not QCommandLineParser: one flag doesn't justify it,
  // and this leaves Qt's own arguments (-style, -platform) alone.
  const bool classic = QApplication::arguments().contains("--classic");

  QMainWindow* window = classic ? static_cast<QMainWindow*>(new MainWindow())
                                : static_cast<QMainWindow*>(new LibraryWindow());
  window->setAttribute(Qt::WA_DeleteOnClose);
  // A no-op on a desktop with no tray — window->close() then means
  // exactly what it always did.
  mira_gui::tray::Attach(window);
  window->show();
  return QApplication::exec();
}
