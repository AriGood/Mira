#include "GameEditForm.h"

#include "../client/JsonMapping.h"
#include "../dialogs/OverridesEditor.h"
#include "GamePresentation.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <filesystem>

namespace mira_gui {

GameEditForm::GameEditForm(std::string id, QWidget* parent) : QWidget(parent), id_(std::move(id)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);

  form_ = new QFormLayout();
  auto* form = form_;
  form->setVerticalSpacing(10);
  form->setHorizontalSpacing(14);
  status_label_ = new QLabel(this);
  install_path_label_ = new QLabel(this);
  install_path_label_->setWordWrap(true);
  install_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  last_error_label_ = new QLabel(this);
  last_error_label_->setWordWrap(true);
  last_error_label_->setStyleSheet("color: #c62828;");
  last_error_label_->hide();

  name_edit_ = new QLineEdit(this);
  args_edit_ = new QLineEdit(this);
  working_dir_edit_ = new QLineEdit(this);
  tags_edit_ = new QLineEdit(this);
  tags_edit_->setPlaceholderText("comma-separated — e.g. hidden, co-op");
  tags_edit_->setToolTip(
      "Free-form labels. \"hidden\" keeps this game out of the library until asked for "
      "(Ctrl+H, or the Hidden filter).");

  exe_combo_ = new QComboBox(this);
  exe_combo_->setEditable(true);
  exe_combo_->setInsertPolicy(QComboBox::NoInsert);
  exe_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect(exe_combo_, QOverload<int>::of(&QComboBox::activated), this,
          &GameEditForm::OnExeComboActivated);

  auto* exe_row = new QHBoxLayout();
  auto* browse_button = new QPushButton("Browse…", this);
  connect(browse_button, &QPushButton::clicked, this, &GameEditForm::BrowseExecutable);
  exe_row->addWidget(exe_combo_, /*stretch=*/1);
  exe_row->addWidget(browse_button);

  runner_combo_ = new QComboBox(this);
  runner_combo_->setEditable(true);
  runner_combo_->setInsertPolicy(QComboBox::NoInsert);
  runner_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  runner_combo_->lineEdit()->setPlaceholderText("Auto (use default runner)");
  connect(runner_combo_, QOverload<int>::of(&QComboBox::activated), this,
          &GameEditForm::OnRunnerComboActivated);
  auto* runner_row = new QHBoxLayout();
  runner_row->addWidget(runner_combo_, /*stretch=*/1);

  form->addRow("Status:", status_label_);
  form->addRow("Install path:", install_path_label_);
  form->addRow("Name:", name_edit_);
  form->addRow("Executable:", exe_row);
  form->addRow("Arguments:", args_edit_);
  form->addRow("Working directory:", working_dir_edit_);
  form->addRow("Tags:", tags_edit_);
  form->addRow("Runner:", runner_row);
  layout->addLayout(form);
  layout->addWidget(last_error_label_);

  show_advanced_ = new QCheckBox("Show advanced (data directory, runner config, overrides)", this);
  connect(show_advanced_, &QCheckBox::toggled, this, &GameEditForm::SetAdvancedVisible);
  layout->addWidget(show_advanced_);

  advanced_container_ = new QWidget(this);
  auto* advanced_layout = new QVBoxLayout(advanced_container_);
  advanced_layout->setContentsMargins(0, 0, 0, 0);
  advanced_layout->setSpacing(10);

  auto* advanced_form = new QFormLayout();
  advanced_form->setVerticalSpacing(10);
  advanced_form->setHorizontalSpacing(14);

  data_dir_edit_ = new QLineEdit(advanced_container_);
  data_dir_edit_->setToolTip("Where this game's prefix/data directory lives.");
  advanced_form->addRow("Data directory:", data_dir_edit_);

  runner_config_edit_ = new QPlainTextEdit(advanced_container_);
  runner_config_edit_->setFixedHeight(70);
  runner_config_edit_->setToolTip("Runner-specific settings, as a JSON object. Merged, not replaced.");
  advanced_form->addRow("Runner config:", runner_config_edit_);

  env_edit_ = new QPlainTextEdit(advanced_container_);
  env_edit_->setFixedHeight(70);
  env_edit_->setToolTip("Extra environment variables, as a JSON object of strings. Merged, not replaced.");
  advanced_form->addRow("Environment:", env_edit_);

  advanced_layout->addLayout(advanced_form);

  overrides_ = new mira_gui::OverridesEditor(id_, advanced_container_);
  advanced_layout->addWidget(overrides_);

  advanced_container_->hide();
  layout->addWidget(advanced_container_, /*stretch=*/1);

  setEnabled(false);
  Load();
}

void GameEditForm::SetAdvancedVisible(bool show) { advanced_container_->setVisible(show); }

void GameEditForm::Load() {
  mira_gui::MiradClient::ListRunnersAsync(this, [this](mira_gui::RunnersResult result) {
    PopulateRunnerCombo(result);
  });
  mira_gui::MiradClient::GetGameAsync(this, id_, [this](mira_gui::GameDetailResult result) {
    if (!result.ok) {
      emit LoadFailed(QString::fromStdString(result.error));
      return;
    }
    setEnabled(true);
    Populate(result.game);
  });
  overrides_->Load();
}

void GameEditForm::Populate(const mira_gui::GameDetail& game) {
  emit Loaded(QString::fromStdString(game.name));

  // "Ready" is the common case and says nothing worth a line of its own —
  // only a state that needs attention earns one.
  form_->setRowVisible(status_label_, game.status != "ready");
  status_label_->setText(QString::fromStdString(game.status));
  status_label_->setStyleSheet(
      QString("color: %1;").arg(mira_gui::StatusColor(game.status).name()));

  install_path_ = game.install_path;
  install_path_label_->setText(QString::fromStdString(game.install_path));

  if (game.last_error.empty()) {
    last_error_label_->hide();
  } else {
    last_error_label_->setText(QString("Error: %1").arg(QString::fromStdString(game.last_error)));
    last_error_label_->show();
  }

  name_edit_->setText(QString::fromStdString(game.name));
  name_edit_->setCursorPosition(0);
  args_edit_->setText(QString::fromStdString(game.args));
  args_edit_->setCursorPosition(0);
  working_dir_edit_->setText(QString::fromStdString(game.working_dir));
  working_dir_edit_->setCursorPosition(0);
  tags_edit_->setText(QString::fromStdString(mira_gui::mapping::ToDisplayString(nlohmann::json(game.tags))));
  tags_edit_->setCursorPosition(0);
  data_dir_edit_->setText(QString::fromStdString(game.data_dir));
  data_dir_edit_->setCursorPosition(0);
  runner_config_edit_->setPlainText(QString::fromStdString(game.runner_config_json));
  env_edit_->setPlainText(QString::fromStdString(game.env_json));

  PopulateExeCombo(game.candidates, game.exe_path);

  runner_combo_->setEditText(QString::fromStdString(game.runner_ref));
  runner_combo_->lineEdit()->setCursorPosition(0);

  original_patch_ = CurrentPatch();
}

void GameEditForm::PopulateExeCombo(const std::vector<mira_gui::GameDetail::Candidate>& candidates,
                                    const std::string& current) {
  std::vector<mira_gui::GameDetail::Candidate> sorted = candidates;
  std::ranges::sort(sorted, std::greater{}, &mira_gui::GameDetail::Candidate::score);

  exe_combo_->blockSignals(true);
  exe_combo_->clear();
  for (const mira_gui::GameDetail::Candidate& candidate : sorted) {
    QString label = QString("%1 — %2, score %3")
                        .arg(QString::fromStdString(candidate.rel_path),
                             QString::fromStdString(candidate.kind))
                        .arg(candidate.score, 0, 'f', 1);
    if (candidate.is_installer) label += " (installer, probably not the game)";
    exe_combo_->addItem(label, QString::fromStdString(candidate.rel_path));
  }
  exe_combo_->blockSignals(false);

  exe_combo_->setEditText(QString::fromStdString(current));
  exe_combo_->lineEdit()->setCursorPosition(0);
}

void GameEditForm::PopulateRunnerCombo(const mira_gui::RunnersResult& result) {
  const QString current = runner_combo_->currentText();
  runner_combo_->blockSignals(true);
  runner_combo_->clear();
  runner_combo_->addItem("Auto (use default runner)", QString());
  if (result.ok) {
    for (const mira_gui::RunnerInfo& runner : result.runners) {
      const QString label = QString("%1 (%2)").arg(QString::fromStdString(runner.name),
                                                     QString::fromStdString(runner.kind));
      runner_combo_->addItem(label, QString::fromStdString(runner.reference));
    }
  }
  runner_combo_->setEditText(current);
  runner_combo_->blockSignals(false);
}

void GameEditForm::OnExeComboActivated(int index) {
  exe_combo_->setEditText(exe_combo_->itemData(index).toString());
  exe_combo_->lineEdit()->setCursorPosition(0);
}

void GameEditForm::OnRunnerComboActivated(int index) {
  runner_combo_->setEditText(runner_combo_->itemData(index).toString());
  runner_combo_->lineEdit()->setCursorPosition(0);
}

void GameEditForm::BrowseExecutable() {
  const QString start_dir = install_path_.empty() ? QString() : QString::fromStdString(install_path_);
  const QString selected = QFileDialog::getOpenFileName(this, "Select executable", start_dir);
  if (selected.isEmpty()) return;

  std::error_code ec;
  const std::filesystem::path relative =
      std::filesystem::relative(selected.toStdString(), install_path_, ec);
  exe_combo_->setEditText(!ec && !relative.empty() ? QString::fromStdString(relative.string())
                                                    : selected);
  exe_combo_->lineEdit()->setCursorPosition(0);
}

mira_gui::GamePatch GameEditForm::CurrentPatch() const {
  mira_gui::GamePatch patch;
  patch.name = name_edit_->text().toStdString();
  patch.exe_path = exe_combo_->currentText().toStdString();
  patch.args = args_edit_->text().toStdString();
  patch.working_dir = working_dir_edit_->text().toStdString();
  patch.tags = mira_gui::mapping::SplitCommaSeparated(tags_edit_->text().toStdString());
  patch.runner_ref = runner_combo_->currentText().toStdString();
  patch.data_dir = data_dir_edit_->text().toStdString();
  patch.runner_config_json = runner_config_edit_->toPlainText().toStdString();
  patch.env_json = env_edit_->toPlainText().toStdString();
  return patch;
}

bool GameEditForm::IsDirty() const {
  const mira_gui::GamePatch current = CurrentPatch();
  return current.name != original_patch_.name || current.exe_path != original_patch_.exe_path ||
         current.args != original_patch_.args ||
         current.working_dir != original_patch_.working_dir ||
         current.tags != original_patch_.tags || current.runner_ref != original_patch_.runner_ref ||
         current.data_dir != original_patch_.data_dir ||
         current.runner_config_json != original_patch_.runner_config_json ||
         current.env_json != original_patch_.env_json || !overrides_->PendingEdits().empty();
}

void GameEditForm::Save() {
  const mira_gui::GamePatch patch = CurrentPatch();
  const std::vector<mira_gui::GameConfigEdit> override_edits = overrides_->PendingEdits();

  setEnabled(false);
  mira_gui::MiradClient::PatchGameAsync(
      this, id_, patch, [this, patch, override_edits](mira_gui::PatchGameResult result) {
        if (!result.ok) {
          setEnabled(true);
          emit SaveFinished(false, QString::fromStdString(result.error));
          return;
        }
        original_patch_ = patch;
        if (override_edits.empty()) {
          setEnabled(true);
          emit SaveFinished(true, QString());
          return;
        }
        mira_gui::MiradClient::PatchGameConfigAsync(
            this, id_, override_edits, [this](mira_gui::PatchGameConfigResult override_result) {
              setEnabled(true);
              if (!override_result.ok) {
                emit SaveFinished(false, QString::fromStdString(override_result.error));
                return;
              }
              overrides_->MarkSaved();
              emit SaveFinished(true, QString());
            });
      });
}

}  // namespace mira_gui
