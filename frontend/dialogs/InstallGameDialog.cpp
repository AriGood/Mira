#include "InstallGameDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

#include "../client/MiradClient.h"

namespace mira_gui {

InstallGameDialog::InstallGameDialog(std::string game_id, const std::string& install_path,
                                     const QString& name, QWidget* parent)
    : QDialog(parent), game_id_(std::move(game_id)), install_path_(install_path) {
  setWindowTitle("Install " + name);
  setMinimumWidth(520);

  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(10);

  info_ = new QLabel("Reading the installer…", this);
  info_->setWordWrap(true);
  info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(info_);

  interactive_ = new QCheckBox("Show the installer window", this);
  interactive_->setToolTip("Click through the installer yourself instead of letting Mira answer it.");
  layout->addWidget(interactive_);

  auto* choose_row = new QHBoxLayout();
  auto* choose = new QPushButton("Choose a different installer…", this);
  connect(choose, &QPushButton::clicked, this, [this] {
    const QString picked = QFileDialog::getOpenFileName(
        this, "Installer", QString::fromStdString(install_path_),
        "Installers (*.exe *.msi *.EXE *.MSI);;All files (*)");
    if (!picked.isEmpty()) LoadInfo(picked.toStdString());
  });
  choose_row->addWidget(choose);
  choose_row->addStretch(1);
  layout->addLayout(choose_row);

  error_ = new QLabel(this);
  error_->setProperty("role", "error");
  error_->setWordWrap(true);
  error_->setVisible(false);
  layout->addWidget(error_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  install_ = buttons->addButton("Install", QDialogButtonBox::AcceptRole);
  install_->setEnabled(false);
  install_->setDefault(true);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(install_, &QPushButton::clicked, this, &InstallGameDialog::Install);
  layout->addWidget(buttons);

  LoadInfo(std::string());
}

void InstallGameDialog::LoadInfo(const std::string& path) {
  install_->setEnabled(false);
  error_->setVisible(false);
  MiradClient::GetInstallerInfoAsync(this, game_id_, path, [this, path](InstallerInfoResult info) {
    if (!info.ok) {
      error_->setText("Could not read the installer: " + QString::fromStdString(info.error));
      error_->setVisible(true);
      return;
    }
    installer_ = path;
    const QString file = QFileInfo(QString::fromStdString(info.path)).fileName();
    const QString size = QLocale().formattedDataSize(info.size_bytes);
    QString how;
    if (info.format == "inno") {
      how = "An Inno Setup installer: Mira runs it without its window.";
    } else if (info.format == "nsis") {
      how = "An NSIS installer: Mira runs it without its window.";
    } else {
      how = "Mira can't answer this installer by itself, so its window opens for you to click "
            "through.";
    }
    info_->setText(QString("<b>%1</b> (%2)<br>%3").arg(file.toHtmlEscaped(), size, how));
    interactive_->setChecked(!info.silent);
    interactive_->setEnabled(info.silent);
    install_->setEnabled(true);
    adjustSize();  // the text above just grew
  });
}

void InstallGameDialog::Install() {
  install_->setEnabled(false);
  error_->setVisible(false);
  MiradClient::InstallGameAsync(this, game_id_, interactive_->isChecked(), installer_,
                                [this](GameActionResult result) {
                                  if (result.ok) {
                                    accept();
                                    return;
                                  }
                                  install_->setEnabled(true);
                                  error_->setText("Could not start the install: " +
                                                  QString::fromStdString(result.error));
                                  error_->setVisible(true);
                                });
}

}  // namespace mira_gui
