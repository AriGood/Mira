#include <QApplication>
#include <QIcon>
#include <QStringList>

#include "views/LibraryWindow.h"
#include "views/MainWindow.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("Mira");
  QApplication::setOrganizationName("mira");

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
  window->show();
  return QApplication::exec();
}
