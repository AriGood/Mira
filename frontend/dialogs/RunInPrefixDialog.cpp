#include "RunInPrefixDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "../client/MiradClient.h"

namespace mira_gui {

RunInPrefixDialog::RunInPrefixDialog(std::string game_id, const GameDetail& game, QWidget* parent)
    : QDialog(parent), game_id_(std::move(game_id)) {
  setWindowTitle("Run in prefix");
  resize(520, 0);

  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(8);

  auto* explanation = new QLabel(
      QString("Runs an executable inside \"%1\"'s own Wine/Proton prefix. If the game has no "
              "prefix yet, one is created first — which is how an installer gets run for a game "
              "that needs installing.")
          .arg(QString::fromStdString(game.name)),
      this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  auto* form = new QFormLayout();
  exe_ = new QComboBox(this);
  exe_->setEditable(true);
  exe_->setInsertPolicy(QComboBox::NoInsert);
  exe_->lineEdit()->setPlaceholderText("Path, absolute or relative to the install folder");

  // Installers first: the common case for this dialog is a needs_install
  // game whose chosen candidate *is* the setup program.
  std::vector<GameDetail::Candidate> candidates = game.candidates;
  std::stable_partition(candidates.begin(), candidates.end(),
                        [](const GameDetail::Candidate& c) { return c.is_installer; });
  for (const GameDetail::Candidate& candidate : candidates) {
    QString label = QString::fromStdString(candidate.rel_path);
    if (candidate.is_installer) label += "  (installer)";
    exe_->addItem(label, QString::fromStdString(candidate.rel_path));
  }
  if (exe_->count() > 0) {
    exe_->setCurrentIndex(0);
    exe_->setEditText(exe_->itemData(0).toString());
  }
  connect(exe_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
    if (index >= 0) exe_->setEditText(exe_->itemData(index).toString());
  });
  form->addRow("Executable", exe_);

  args_ = new QLineEdit(this);
  args_->setPlaceholderText("Optional, e.g. /S for a silent install");
  form->addRow("Arguments", args_);
  layout->addLayout(form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  run_ = buttons->addButton("Run", QDialogButtonBox::AcceptRole);
  connect(run_, &QPushButton::clicked, this, &RunInPrefixDialog::Run);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

void RunInPrefixDialog::Run() {
  const std::string exe = exe_->currentText().toStdString();
  if (exe.empty()) {
    QMessageBox::warning(this, "Run", "Pick an executable to run.");
    return;
  }

  // Provisioning a prefix on demand is genuinely slow (it's initialising
  // Wine/Proton), and the request doesn't return until the process exists,
  // so the dialog says what it's doing rather than appearing frozen.
  setEnabled(false);
  run_->setText("Starting…");
  MiradClient::RunInPrefixAsync(this, game_id_, exe, args_->text().toStdString(),
                                [this](RunInPrefixResult result) {
                                  setEnabled(true);
                                  run_->setText("Run");
                                  if (!result.ok) {
                                    QMessageBox::warning(this, "Run failed",
                                                         QString::fromStdString(result.error));
                                    return;
                                  }
                                  accept();
                                });
}

}  // namespace mira_gui
