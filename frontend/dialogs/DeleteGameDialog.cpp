#include "DeleteGameDialog.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace mira_gui {
namespace {

// One destructive option: a checkbox plus the path it would delete.
QCheckBox* AddPathOption(QVBoxLayout* layout, QWidget* parent, const QString& label,
                         const QString& path, const QString& empty_reason) {
  auto* check = new QCheckBox(label, parent);
  layout->addWidget(check);

  auto* path_label = new QLabel(parent);
  path_label->setWordWrap(true);
  path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  path_label->setContentsMargins(22, 0, 0, 6);
  path_label->setProperty("role", "muted");
  layout->addWidget(path_label);

  if (path.isEmpty()) {
    check->setEnabled(false);
    check->setToolTip(empty_reason);
    path_label->setText(empty_reason);
  } else {
    path_label->setText(path);
    path_label->setToolTip(path);
  }
  return check;
}

}  // namespace

DeleteChoice AskDeleteGame(QWidget* parent, const QString& name, const QString& install_path,
                           const QString& data_dir) {
  QDialog dialog(parent);
  dialog.setWindowTitle("Remove game");
  dialog.setModal(true);

  auto* layout = new QVBoxLayout(&dialog);
  layout->setSpacing(8);

  auto* heading = new QLabel(QString("Remove \"%1\" from the library?").arg(name), &dialog);
  heading->setWordWrap(true);
  heading->setProperty("role", "section");
  layout->addWidget(heading);

  auto* explanation = new QLabel(
      "Mira forgets the game. Nothing on disk is touched unless you tick an option below.",
      &dialog);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  QCheckBox* files_check =
      AddPathOption(layout, &dialog, "Also delete the game's files", install_path,
                    "This game has no install path on record.");
  QCheckBox* prefix_check =
      AddPathOption(layout, &dialog, "Also delete its Wine/Proton prefix", data_dir,
                    "This game has no prefix (native games don't need one).");

  auto* warning = new QLabel(
      "⚠ Deleting files cannot be undone. mirad will refuse any path that isn't inside a "
      "configured library root or prefix root.",
      &dialog);
  warning->setWordWrap(true);
  warning->setProperty("role", "error");
  warning->setVisible(false);
  layout->addWidget(warning);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
  QPushButton* remove_button = buttons->addButton("Remove", QDialogButtonBox::AcceptRole);
  // Cancel keeps the focus: this dialog is reached by clicking a Delete
  // button, so Enter should not be a second confirmation of the same intent.
  buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);

  // The button text follows the checkboxes, so the last thing read before
  // clicking says whether this deletes data or only a library entry.
  auto sync_buttons = [&] {
    const bool destructive = files_check->isChecked() || prefix_check->isChecked();
    warning->setVisible(destructive);
    remove_button->setText(destructive ? "Delete" : "Remove");
  };
  QObject::connect(files_check, &QCheckBox::toggled, &dialog, sync_buttons);
  QObject::connect(prefix_check, &QCheckBox::toggled, &dialog, sync_buttons);
  sync_buttons();

  DeleteChoice choice;
  if (dialog.exec() != QDialog::Accepted) return choice;
  choice.confirmed = true;
  choice.delete_files = files_check->isChecked();
  choice.delete_prefix = prefix_check->isChecked();
  return choice;
}

}  // namespace mira_gui
