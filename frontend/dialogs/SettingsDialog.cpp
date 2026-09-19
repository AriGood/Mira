#include "SettingsDialog.h"

#include "../ui/Notify.h"
#include "../ui/SettingsPanel.h"

#include <QDialogButtonBox>
#include <QPushButton>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Settings");
  resize(640, 620);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(10);

  panel_ = new mira_gui::SettingsPanel(this);
  connect(panel_, &mira_gui::SettingsPanel::LoadFailed, this, [this](QString error) {
    mira_gui::notify::Failed(this, "Could not load the settings.", error);
    reject();
  });
  connect(panel_, &mira_gui::SettingsPanel::SaveFinished, this, [this](bool ok, QString error) {
    if (!ok) {
      mira_gui::notify::Failed(this, "Could not save the settings.", error);
      return;
    }
    accept();
  });
  layout->addWidget(panel_, /*stretch=*/1);

  auto* buttons = new QDialogButtonBox(this);
  QPushButton* save_button = buttons->addButton("Save", QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Close);
  connect(save_button, &QPushButton::clicked, panel_, &mira_gui::SettingsPanel::Save);
  connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
  layout->addWidget(buttons);
}

void SettingsDialog::reject() {
  if (!panel_->IsDirty()) {
    QDialog::reject();
    return;
  }
  switch (mira_gui::notify::ConfirmUnsaved(this, "Settings changed but not saved.")) {
    case mira_gui::notify::UnsavedAction::Cancel:
      return;
    case mira_gui::notify::UnsavedAction::SaveAndExit:
      panel_->Save();  // SaveFinished, connected above, calls accept() on success
      return;
    case mira_gui::notify::UnsavedAction::DiscardAndExit:
      QDialog::reject();
      return;
  }
}
