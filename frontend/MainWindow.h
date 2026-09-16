#pragma once

#include <QMainWindow>

// Deliberately blank: the real frontend (library grid, config menu, tray
// integration, daemon lifecycle handling) is separate follow-on work. This
// class exists so the frontend has a CMake target, a window, and a desktop
// entry to build on, per docs/architecture.md.
class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget* parent = nullptr);
};
