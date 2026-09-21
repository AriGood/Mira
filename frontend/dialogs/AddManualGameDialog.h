#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;

namespace mira_gui {

// POST /v1/games/manual: point Mira at a game (or its installer) outside
// every configured library root.
class AddManualGameDialog : public QDialog {
  Q_OBJECT

public:
  explicit AddManualGameDialog(QWidget* parent = nullptr);

private:
  void BrowseExe();
  void Submit();

  QLineEdit* install_path_ = nullptr;
  QLineEdit* exe_path_ = nullptr;
  QLineEdit* name_ = nullptr;
  QComboBox* platform_ = nullptr;
  QCheckBox* is_installer_ = nullptr;
  QPushButton* add_ = nullptr;
};

}  // namespace mira_gui
