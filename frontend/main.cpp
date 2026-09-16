#include <QApplication>

#include "MainWindow.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("Mira");
  QApplication::setOrganizationName("mira");

  MainWindow window;
  window.show();
  return QApplication::exec();
}
