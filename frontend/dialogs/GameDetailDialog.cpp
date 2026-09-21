#include "GameDetailDialog.h"

#include "../ui/GameEditForm.h"
#include "../ui/Notify.h"

#include <QDialogButtonBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

GameDetailDialog::GameDetailDialog(std::string id, QWidget* parent, mira_gui::ArtworkStore* artwork)
    : QDialog(parent) {
  setWindowTitle("Loading…");
  // 780x640, not the old 560x480: wide enough for the form's fields next to
  // their labels, tall enough that the QScrollArea below rarely has to
  // scroll.
  resize(780, 640);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(12);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  form_ = new mira_gui::GameEditForm(std::move(id), scroll);
  form_->SetArtworkStore(artwork);
  scroll->setWidget(form_);
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
  layout->addWidget(scroll, /*stretch=*/1);

  auto* buttons = new QDialogButtonBox(this);
  save_button_ = buttons->addButton("Save", QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);
  connect(save_button_, &QPushButton::clicked, form_, &mira_gui::GameEditForm::Save);
  connect(buttons, &QDialogButtonBox::rejected, this, &GameDetailDialog::reject);
  layout->addWidget(buttons);
}

void GameDetailDialog::reject() {
  if (!form_->IsDirty()) {
    QDialog::reject();
    return;
  }
  switch (mira_gui::notify::ConfirmUnsaved(this, "This game's edits aren't saved.")) {
    case mira_gui::notify::UnsavedAction::Cancel:
      return;
    case mira_gui::notify::UnsavedAction::SaveAndExit:
      form_->Save();  // SaveFinished, connected above, calls accept() on success
      return;
    case mira_gui::notify::UnsavedAction::DiscardAndExit:
      QDialog::reject();
      return;
  }
}
