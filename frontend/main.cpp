#include <QApplication>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStringList>

#include "ui/DaemonSupervisor.h"
#include "ui/SystemNotifier.h"
#include "ui/Tray.h"
#include "views/LibraryWindow.h"
#include "views/MainWindow.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("Mira");
  QApplication::setOrganizationName("mira");

  // Mira closes to tray rather than quitting, so a second launch (another
  // double-click on the AppImage, another "Mira" from the app menu) must
  // not open a second window against the same daemon — it should just no-op.
  // QLockFile detects and clears a lock left by a crashed instance on its
  // own (it checks whether the PID that holds it is still alive).
  const QString runtime_dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/mira";
  QDir().mkpath(runtime_dir);
  QLockFile single_instance_lock(runtime_dir + "/mira-gui.lock");
  if (!single_instance_lock.tryLock(0)) {
    return 0;
  }
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

  // docs/architecture.md's "frontend-managed" daemon path: start mirad
  // ourselves if nothing is already listening, so the AppImage works as one
  // self-contained app with no systemd unit required.
  auto* supervisor = new mira_gui::DaemonSupervisor(&app);
  QObject::connect(supervisor, &mira_gui::DaemonSupervisor::Ready, &app, [classic] {
    QMainWindow* window = classic ? static_cast<QMainWindow*>(new MainWindow())
                                  : static_cast<QMainWindow*>(new LibraryWindow());
    window->setAttribute(Qt::WA_DeleteOnClose);
    // A no-op on a desktop with no tray (Tray.cpp) — window->close() then
    // means exactly what it always did.
    mira_gui::tray::Attach(window);
    window->show();
  });
  QObject::connect(supervisor, &mira_gui::DaemonSupervisor::Failed, &app, [](QString error) {
    QMessageBox::critical(nullptr, "Mira", "Could not start mirad: " + error);
    QApplication::quit();
  });
  supervisor->EnsureRunning();

  return QApplication::exec();
}
