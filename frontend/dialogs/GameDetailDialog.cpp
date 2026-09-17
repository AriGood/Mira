#include "GameDetailDialog.h"

#include "../ui/GameEditForm.h"
#include "../ui/Notify.h"

#include <QDialogButtonBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

GameDetailDialog::GameDetailDialog(std::string id, QWidget* parent) : QDialog(parent) {
  setWindowTitle("Loading…");
  resize(560, 460);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(12);

  form_ = new mira_gui::GameEditForm(std::move(id), this);
  connect(form_, &mira_gui::GameEditForm::Loaded, this,
          [this](QString name) { setWindowTitle(name); });
  connect(form_, &mira_gui::GameEditForm::LoadFailed, this, [this](QString error) {
    mira_gui::notify::Failed(this, "Could not load this game.", error);
    reject();
  });
  connect(form_, &mira_gui::GameEditForm::SaveFinished, this, [this](bool ok, QString error) {
    if (!ok) {
      mira_gui::notify::Failed(this, "Could not save this game.", error);
      return;
    }
    accept();
  });
  layout->addWidget(form_, /*stretch=*/1);

  auto* buttons = new QDialogButtonBox(this);
  save_button_ = buttons->addButton("Save", QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);
  connect(save_button_, &QPushButton::clicked, form_, &mira_gui::GameEditForm::Save);
  connect(buttons, &QDialogButtonBox::rejected, this, &GameDetailDialog::reject);
  layout->addWidget(buttons);
}
