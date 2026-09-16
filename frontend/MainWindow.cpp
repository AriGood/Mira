#include "MainWindow.h"

#include <QLabel>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("Mira");
  resize(900, 600);

  auto* placeholder = new QLabel(
      "Mira\n\n"
      "The frontend is not implemented yet — this window exists so the\n"
      "project has something to build on. The backend (mirad) is a separate\n"
      "process reachable over docs/api.md.",
      this);
  placeholder->setAlignment(Qt::AlignCenter);
  setCentralWidget(placeholder);
}
