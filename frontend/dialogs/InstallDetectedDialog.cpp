#include "InstallDetectedDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <filesystem>

namespace mira_gui {
namespace {

// The path as Windows programs see it: what follows the prefix's drive_c.
QString WindowsPath(const std::string& path) {
  const QString text = QString::fromStdString(path);
  const int at = text.indexOf("/drive_c/");
  return at < 0 ? text : "C:" + text.mid(at + 8).replace('/', '\\');
}

}  // namespace

InstallDetectedDialog::InstallDetectedDialog(const QString& name, const std::string& install_path,
                                             const std::string& exe_path, QWidget* parent)
    : QDialog(parent), install_path_(install_path), exe_path_(exe_path) {
  setWindowTitle(name + " looks like an installer");
  setMinimumWidth(480);

  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(10);
  auto* text = new QLabel(QString("Running it installed something into %1. Use that program instead of the "
                                  "installer?")
                              .arg(WindowsPath(install_path)),
                          this);
  text->setWordWrap(true);
  layout->addWidget(text);
  app_ = new QCheckBox("This is an app, not a game", this);
  layout->addWidget(app_);

  auto* buttons = new QDialogButtonBox(this);
  if (!exe_path_.empty()) {
    auto* use = buttons->addButton("Use installed program", QDialogButtonBox::AcceptRole);
    use->setDefault(true);
  }
  auto* choose = buttons->addButton("Choose program…", QDialogButtonBox::ActionRole);
  buttons->addButton("Keep as is", QDialogButtonBox::RejectRole);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(choose, &QPushButton::clicked, this, [this] {
    const QString selected = QFileDialog::getOpenFileName(this, "Select the installed program",
                                                          QString::fromStdString(install_path_),
                                                          "Programs (*.exe);;All files (*)");
    if (selected.isEmpty()) return;
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(selected.toStdString(), install_path_, ec);
    if (ec || relative.empty() || relative.native().starts_with("..")) return;  // outside the folder
    exe_path_ = relative.string();
    accept();
  });
  layout->addWidget(buttons);
}

bool InstallDetectedDialog::IsApp() const { return app_->isChecked(); }

}  // namespace mira_gui
