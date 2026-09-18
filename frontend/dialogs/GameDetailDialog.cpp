#include "GameDetailDialog.h"

#include "OverridesEditor.h"

#include "../ui/GamePresentation.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>

#include "../ui/Notify.h"
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <filesystem>

GameDetailDialog::GameDetailDialog(std::string id, QWidget* parent)
    : QDialog(parent), id_(std::move(id)) {
  setWindowTitle("Loading…");
  resize(560, 460);

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
  args_edit_ = new QLineEdit(this);
  working_dir_edit_ = new QLineEdit(this);

  // Editable so a path can be typed directly; its dropdown also offers every
  // detected candidate labeled with its score (see PopulateExeCombo).
  // Selecting one sets the edit text to the plain path, not the descriptive
  // label — see OnExeComboActivated. QComboBox locks its width to whatever
  // its widest item was at first show unless told otherwise, so it's forced
  // to fill the row the same way the line edits above it do: an explicit
  // Expanding policy plus a stretch factor in its row, not just addRow.
  exe_combo_ = new QComboBox(this);
  exe_combo_->setEditable(true);
  exe_combo_->setInsertPolicy(QComboBox::NoInsert);
  exe_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect(exe_combo_, QOverload<int>::of(&QComboBox::activated), this,
          &GameDetailDialog::OnExeComboActivated);

  auto* exe_row = new QHBoxLayout();
  auto* browse_button = new QPushButton("Browse…", this);
  connect(browse_button, &QPushButton::clicked, this, &GameDetailDialog::BrowseExecutable);
  exe_row->addWidget(exe_combo_, /*stretch=*/1);
  exe_row->addWidget(browse_button);

  // Same pattern as the exe combo, populated from GET /v1/runners so a
  // runner reads by name ("GE-Proton11-7") instead of the raw "kind:name"
  // string, while still allowing that raw string to be typed by hand.
  runner_combo_ = new QComboBox(this);
  runner_combo_->setEditable(true);
  runner_combo_->setInsertPolicy(QComboBox::NoInsert);
  runner_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  // An empty runner_ref means "use the default" (docs/api.md) — its actual
  // value has to stay "" for that to round-trip through Save() correctly,
  // so the friendly label lives in the placeholder rather than being typed
  // into the field: Qt shows it automatically whenever the field is empty,
  // for both the initial value and picking "Auto" back from the dropdown.
  runner_combo_->lineEdit()->setPlaceholderText("Auto (use default runner)");
  connect(runner_combo_, QOverload<int>::of(&QComboBox::activated), this,
          &GameDetailDialog::OnRunnerComboActivated);
  auto* runner_row = new QHBoxLayout();
  runner_row->addWidget(runner_combo_, /*stretch=*/1);

  form->addRow("Status:", status_label_);
  form->addRow("Confidence:", confidence_label_);
  form->addRow("Install path:", install_path_label_);
  form->addRow("Name:", name_edit_);
  form->addRow("Executable:", exe_row);
  form->addRow("Arguments:", args_edit_);
  form->addRow("Working directory:", working_dir_edit_);
  form->addRow("Runner:", runner_row);
  layout->addLayout(form);
  layout->addWidget(last_error_label_);

  show_advanced_ = new QCheckBox("Show advanced (data directory, runner config, overrides)", this);
  connect(show_advanced_, &QCheckBox::toggled, this, &GameDetailDialog::SetAdvancedVisible);
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

  // `runner_config`/`env` are arbitrary JSON objects (docs/api.md) with no
  // fixed shape a form could render — a raw JSON text box, parsed on save
  // (MiradClient.cpp), is what "advanced" means for these two.
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
  layout->addStretch(1);

  auto* buttons = new QDialogButtonBox(this);
  save_button_ = buttons->addButton("Save", QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);
  connect(save_button_, &QPushButton::clicked, this, &GameDetailDialog::Save);
  connect(buttons, &QDialogButtonBox::rejected, this, &GameDetailDialog::reject);
  layout->addWidget(buttons);

  setEnabled(false);
  Load();
}

void GameDetailDialog::SetAdvancedVisible(bool show) {
  advanced_container_->setVisible(show);
}

void GameDetailDialog::Load() {
  mira_gui::MiradClient::ListRunnersAsync(this, [this](mira_gui::RunnersResult result) {
    PopulateRunnerCombo(result);
  });
  mira_gui::MiradClient::GetGameAsync(this, id_, [this](mira_gui::GameDetailResult result) {
    if (!result.ok) {
      mira_gui::notify::Failed(this, "Could not load this game.",
                            QString::fromStdString(result.error));
      reject();
      return;
    }
    setEnabled(true);
    Populate(result.game);
  });
  overrides_->Load();
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

  // A value longer than its field would otherwise leave the cursor (and so
  // the visible scroll position) at the end, hiding the start — the part
  // that actually identifies a path or command line.
  name_edit_->setText(QString::fromStdString(game.name));
  name_edit_->setCursorPosition(0);
  args_edit_->setText(QString::fromStdString(game.args));
  args_edit_->setCursorPosition(0);
  working_dir_edit_->setText(QString::fromStdString(game.working_dir));
  working_dir_edit_->setCursorPosition(0);
  data_dir_edit_->setText(QString::fromStdString(game.data_dir));
  data_dir_edit_->setCursorPosition(0);
  runner_config_edit_->setPlainText(QString::fromStdString(game.runner_config_json));
  env_edit_->setPlainText(QString::fromStdString(game.env_json));

  PopulateExeCombo(game.candidates, game.exe_path);

  // Doesn't stomp whatever PopulateRunnerCombo already put in the edit text
  // if it ran first — this only sets the text, never touches the item list,
  // and vice versa (see PopulateRunnerCombo), so the two async calls in
  // Load() can land in either order with the same result.
  runner_combo_->setEditText(QString::fromStdString(game.runner_ref));
  runner_combo_->lineEdit()->setCursorPosition(0);

  original_patch_.name = game.name;
  original_patch_.exe_path = game.exe_path;
  original_patch_.args = game.args;
  original_patch_.working_dir = game.working_dir;
  original_patch_.runner_ref = game.runner_ref;
  original_patch_.data_dir = game.data_dir;
  original_patch_.runner_config_json = game.runner_config_json;
  original_patch_.env_json = game.env_json;
}

void GameDetailDialog::PopulateExeCombo(
    const std::vector<mira_gui::GameDetail::Candidate>& candidates, const std::string& current) {
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

void GameDetailDialog::PopulateRunnerCombo(const mira_gui::RunnersResult& result) {
  // clear() + addItem() resets the displayed text to the first item added —
  // that's the widget's own state, not a signal, so blockSignals() below
  // doesn't prevent it. This and Populate() (from GetGameAsync) can resolve
  // in either order, so whatever's already showing has to survive a
  // repopulate regardless of which one got here first — otherwise a game
  // with a real runner_ref set can have it silently clobbered back to
  // "Auto" just because the runner list happened to arrive second.
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

void GameDetailDialog::OnExeComboActivated(int index) {
  exe_combo_->setEditText(exe_combo_->itemData(index).toString());
  exe_combo_->lineEdit()->setCursorPosition(0);
}

void GameDetailDialog::OnRunnerComboActivated(int index) {
  runner_combo_->setEditText(runner_combo_->itemData(index).toString());
  runner_combo_->lineEdit()->setCursorPosition(0);
}

void GameDetailDialog::BrowseExecutable() {
  const QString start_dir = install_path_.empty() ? QString() : QString::fromStdString(install_path_);
  const QString selected =
      QFileDialog::getOpenFileName(this, "Select executable", start_dir);
  if (selected.isEmpty()) return;

  std::error_code ec;
  const std::filesystem::path relative =
      std::filesystem::relative(selected.toStdString(), install_path_, ec);
  exe_combo_->setEditText(!ec && !relative.empty() ? QString::fromStdString(relative.string())
                                                    : selected);
  exe_combo_->lineEdit()->setCursorPosition(0);
}

void GameDetailDialog::Save() {
  mira_gui::GamePatch patch;
  patch.name = name_edit_->text().toStdString();
  patch.exe_path = exe_combo_->currentText().toStdString();
  patch.args = args_edit_->text().toStdString();
  patch.working_dir = working_dir_edit_->text().toStdString();
  patch.runner_ref = runner_combo_->currentText().toStdString();
  patch.data_dir = data_dir_edit_->text().toStdString();
  patch.runner_config_json = runner_config_edit_->toPlainText().toStdString();
  patch.env_json = env_edit_->toPlainText().toStdString();

  const std::vector<mira_gui::GameConfigEdit> override_edits = overrides_->PendingEdits();

  setEnabled(false);
  mira_gui::MiradClient::PatchGameAsync(this, id_, patch, [this, override_edits](mira_gui::PatchGameResult result) {
    if (!result.ok) {
      setEnabled(true);
      mira_gui::notify::Failed(this, "Could not save this game.",
                               QString::fromStdString(result.error));
      return;
    }
    if (override_edits.empty()) {
      setEnabled(true);
      accept();
      return;
    }
    mira_gui::MiradClient::PatchGameConfigAsync(
        this, id_, override_edits, [this](mira_gui::PatchGameConfigResult override_result) {
          if (!override_result.ok) {
            // Roll back the game-fields PATCH that already landed; best-effort.
            mira_gui::MiradClient::PatchGameAsync(this, id_, original_patch_,
                                                  [](mira_gui::PatchGameResult) {});
            setEnabled(true);
            mira_gui::notify::Failed(
                this, "Could not save this game's overrides — reverted the other changes too.",
                QString::fromStdString(override_result.error));
            return;
          }
          setEnabled(true);
          accept();
        });
  });
}
