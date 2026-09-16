#include "GameDetailDialog.h"

#include "GameColors.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>

GameDetailDialog::GameDetailDialog(std::string id, QWidget* parent)
    : QDialog(parent), id_(std::move(id)) {
  setWindowTitle("Loading…");
  resize(560, 500);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(12);

  auto* form = new QFormLayout();
  form->setVerticalSpacing(10);
  form->setHorizontalSpacing(14);
  status_label_ = new QLabel(this);
  install_path_label_ = new QLabel(this);
  install_path_label_->setWordWrap(true);
  install_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  confidence_label_ = new QLabel(this);
  last_error_label_ = new QLabel(this);
  last_error_label_->setWordWrap(true);
  last_error_label_->setStyleSheet("color: #c62828;");
  last_error_label_->hide();

  name_edit_ = new QLineEdit(this);
  exe_path_edit_ = new QLineEdit(this);
  args_edit_ = new QLineEdit(this);
  working_dir_edit_ = new QLineEdit(this);
  runner_ref_edit_ = new QLineEdit(this);

  // The candidate list's ★ tracks whatever this field currently says, not
  // the detector's original pick — see RenderCandidates.
  connect(exe_path_edit_, &QLineEdit::textChanged, this, &GameDetailDialog::RenderCandidates);

  auto* exe_row = new QHBoxLayout();
  auto* browse_button = new QPushButton("Browse…", this);
  connect(browse_button, &QPushButton::clicked, this, &GameDetailDialog::BrowseExecutable);
  exe_row->addWidget(exe_path_edit_, /*stretch=*/1);
  exe_row->addWidget(browse_button);

  form->addRow("Status:", status_label_);
  form->addRow("Confidence:", confidence_label_);
  form->addRow("Install path:", install_path_label_);
  form->addRow("Name:", name_edit_);
  form->addRow("Executable:", exe_row);
  form->addRow("Arguments:", args_edit_);
  form->addRow("Working directory:", working_dir_edit_);
  form->addRow("Runner:", runner_ref_edit_);
  layout->addLayout(form);
  layout->addWidget(last_error_label_);

  auto* candidates_label = new QLabel("Detected candidates:", this);
  candidates_label->setStyleSheet("font-weight: 600;");
  layout->addWidget(candidates_label);

  auto* candidates_container = new QWidget(this);
  candidates_layout_ = new QVBoxLayout(candidates_container);
  candidates_layout_->setSpacing(8);
  candidates_layout_->setContentsMargins(2, 2, 2, 2);

  // Capped, not unbounded: a game with a handful of candidates shouldn't be
  // cramped, but one with dozens (a big Windows install tree) shouldn't blow
  // the dialog out past the screen either — it scrolls instead.
  auto* candidates_scroll = new QScrollArea(this);
  candidates_scroll->setWidget(candidates_container);
  candidates_scroll->setWidgetResizable(true);
  candidates_scroll->setMaximumHeight(220);
  candidates_scroll->setFrameShape(QFrame::NoFrame);
  layout->addWidget(candidates_scroll, /*stretch=*/1);

  auto* buttons = new QDialogButtonBox(this);
  save_button_ = buttons->addButton("Save", QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);
  connect(save_button_, &QPushButton::clicked, this, &GameDetailDialog::Save);
  connect(buttons, &QDialogButtonBox::rejected, this, &GameDetailDialog::reject);
  layout->addWidget(buttons);

  setEnabled(false);
  Load();
}

void GameDetailDialog::Load() {
  mira_gui::MiradClient::GetGameAsync(this, id_, [this](mira_gui::GameDetailResult result) {
    if (!result.ok) {
      QMessageBox::warning(this, "Failed to load game",
                            QString::fromStdString(result.error));
      reject();
      return;
    }
    setEnabled(true);
    Populate(result.game);
  });
}

void GameDetailDialog::Populate(const mira_gui::GameDetail& game) {
  setWindowTitle(QString::fromStdString(game.name));

  status_label_->setText(QString::fromStdString(game.status));
  status_label_->setStyleSheet(
      QString("color: %1;").arg(mira_gui::StatusColor(game.status).name()));

  confidence_label_->setText(mira_gui::ConfidenceText(game.reviewed, game.confidence));
  confidence_label_->setStyleSheet(
      QString("color: %1;").arg(mira_gui::ConfidenceColor(game.reviewed, game.confidence).name()));

  install_path_ = game.install_path;
  install_path_label_->setText(QString::fromStdString(game.install_path));

  if (game.last_error.empty()) {
    last_error_label_->hide();
  } else {
    last_error_label_->setText(QString("Error: %1").arg(QString::fromStdString(game.last_error)));
    last_error_label_->show();
  }

  name_edit_->setText(QString::fromStdString(game.name));
  args_edit_->setText(QString::fromStdString(game.args));
  working_dir_edit_->setText(QString::fromStdString(game.working_dir));
  runner_ref_edit_->setText(QString::fromStdString(game.runner_ref));

  candidates_ = game.candidates;
  exe_path_edit_->setText(QString::fromStdString(game.exe_path));  // triggers RenderCandidates
}

void GameDetailDialog::RenderCandidates() {
  QLayoutItem* child;
  while ((child = candidates_layout_->takeAt(0)) != nullptr) {
    delete child->widget();
    delete child;
  }
  if (candidates_.empty()) {
    candidates_layout_->addWidget(new QLabel("(none found)", this));
    return;
  }

  // The star tracks the Executable field's current value, not the
  // detector's original `chosen` flag: once the user picks something else
  // (Browse, or a different candidate), that original pick is no longer
  // what's actually selected and shouldn't still look like it is.
  const std::string current = exe_path_edit_->text().toStdString();
  for (const mira_gui::GameDetail::Candidate& candidate : candidates_) {
    const bool active = candidate.rel_path == current;
    auto* row = new QHBoxLayout();
    row->setSpacing(10);
    const QString marker = active ? "★" : "☆";
    auto* label = new QLabel(
        QString("%1 %2\n%3, score %4")
            .arg(marker, QString::fromStdString(candidate.rel_path),
                 QString::fromStdString(candidate.kind))
            .arg(candidate.score),
        this);
    label->setWordWrap(true);
    if (!active) label->setStyleSheet("color: #9e9e9e;");
    auto* use_button = new QPushButton("Use", this);
    use_button->setMaximumWidth(64);
    use_button->setEnabled(!active);
    const std::string rel_path = candidate.rel_path;
    connect(use_button, &QPushButton::clicked, this, [this, rel_path] { UseCandidate(rel_path); });
    row->addWidget(label, /*stretch=*/1);
    row->addWidget(use_button, 0, Qt::AlignTop);
    candidates_layout_->addLayout(row);
  }
}

void GameDetailDialog::UseCandidate(const std::string& rel_path) {
  exe_path_edit_->setText(QString::fromStdString(rel_path));
}

void GameDetailDialog::BrowseExecutable() {
  const QString start_dir = install_path_.empty() ? QString() : QString::fromStdString(install_path_);
  const QString selected =
      QFileDialog::getOpenFileName(this, "Select executable", start_dir);
  if (selected.isEmpty()) return;

  std::error_code ec;
  const std::filesystem::path relative =
      std::filesystem::relative(selected.toStdString(), install_path_, ec);
  exe_path_edit_->setText(!ec && !relative.empty() ? QString::fromStdString(relative.string())
                                                    : selected);
}

void GameDetailDialog::Save() {
  mira_gui::GamePatch patch;
  patch.name = name_edit_->text().toStdString();
  patch.exe_path = exe_path_edit_->text().toStdString();
  patch.args = args_edit_->text().toStdString();
  patch.working_dir = working_dir_edit_->text().toStdString();
  patch.runner_ref = runner_ref_edit_->text().toStdString();

  setEnabled(false);
  mira_gui::MiradClient::PatchGameAsync(
      this, id_, patch, [this](mira_gui::PatchGameResult result) {
        setEnabled(true);
        if (!result.ok) {
          QMessageBox::warning(this, "Save failed", QString::fromStdString(result.error));
          return;
        }
        accept();
      });
}
