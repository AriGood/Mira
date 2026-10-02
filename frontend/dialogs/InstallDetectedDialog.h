#pragma once

#include <QDialog>
#include <QString>

#include <string>

class QCheckBox;

namespace mira_gui {

// Asked after a launched game turned out to be an installer: switch the entry
// to the program it installed.
class InstallDetectedDialog : public QDialog {
  Q_OBJECT

public:
  // `exe_path` is relative to `install_path`; empty when no program was found.
  InstallDetectedDialog(const QString& name, const std::string& install_path, const std::string& exe_path,
                        QWidget* parent = nullptr);

  // The program to use, relative to the install folder, once accepted.
  std::string ExePath() const { return exe_path_; }
  bool IsApp() const;

private:
  std::string install_path_;
  std::string exe_path_;
  QCheckBox* app_ = nullptr;
};

}  // namespace mira_gui
