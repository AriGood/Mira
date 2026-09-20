#include "GameEditForm.h"

#include "../client/JsonMapping.h"
#include "../dialogs/OverridesEditor.h"
#include "GamePresentation.h"
#include "HeroArtWidget.h"
#include "Theme.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
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

  hero_art_ = new HeroArtWidget(this);
  layout->addWidget(hero_art_);

  form_ = new QFormLayout();
  auto* form = form_;
  form->setVerticalSpacing(10);
  form->setHorizontalSpacing(14);
  // QLabel's default vertical policy is Preferred, not Fixed like
  // QLineEdit/QComboBox, so leftover QScrollArea height landed on whichever
  // one-line read-only label was still willing to grow. Pinned Fixed.
  status_label_ = new QLabel(this);
  status_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  install_path_label_ = new QLabel(this);
  // Also Ignored horizontally: a path has no spaces to word-wrap at, so its
  // minimumSizeHint was the whole string — the sidebar's own floor.
  install_path_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  install_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  last_error_label_ = new QLabel(this);
  last_error_label_->setWordWrap(true);
  last_error_label_->setProperty("role", "error");
  last_error_label_->hide();

  // Set from game.source in Populate() -- hidden for an ordinary scan,
  // where exe_path really is what runs. For Steam it usually isn't.
  source_note_label_ = new QLabel(this);
  source_note_label_->setWordWrap(true);
  source_note_label_->setProperty("role", "muted");
  source_note_label_->hide();

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

  // A QWidget wrapper, not a bare QHBoxLayout passed to addRow(): QFormLayout
  // sizes a nested-layout row from its shortest child, not the tallest,
  // which is what clipped Browse's text in RunInPrefixDialog's copy of this.
  auto* exe_row_widget = new QWidget(this);
  auto* exe_row = new QHBoxLayout(exe_row_widget);
  exe_row->setContentsMargins(0, 0, 0, 0);
  exe_row->setSpacing(8);
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
  auto* runner_row_widget = new QWidget(this);
  auto* runner_row = new QHBoxLayout(runner_row_widget);
  runner_row->setContentsMargins(0, 0, 0, 0);
  runner_row->addWidget(runner_combo_, /*stretch=*/1);

  data_dir_edit_ = new QLineEdit(this);
  data_dir_edit_->setToolTip("Where this game's prefix/data directory lives.");

  runner_config_edit_ = new QPlainTextEdit(this);
  runner_config_edit_->setFixedHeight(70);
  runner_config_edit_->setToolTip("Runner-specific settings, as a JSON object. Merged, not replaced.");

  env_edit_ = new QPlainTextEdit(this);
  // Taller than runner_config_edit_'s 70: env vars are usually several
  // KEY=value-shaped entries, one per line, and 70px only showed two of
  // them at a time before scrolling took over.
  env_edit_->setFixedHeight(110);
  env_edit_->setToolTip("Extra environment variables, as a JSON object of strings. Merged, not replaced.");

  // addRow(QString, ...)'s own label defaults to Preferred vertical, same
  // bug as status_label_/install_path_label_ but for every row's caption —
  // built explicitly and pinned Fixed instead, or the whole form pads out.
  auto add_row = [form, this](const QString& text, auto* field) {
    auto* label = new QLabel(text, this);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    form->addRow(label, field);
  };
  add_row("Status:", status_label_);
  add_row("Install path:", install_path_label_);
  add_row("Name:", name_edit_);
  form->addRow(source_note_label_);
  add_row("Executable:", exe_row_widget);
  add_row("Arguments:", args_edit_);
  add_row("Working directory:", working_dir_edit_);
  add_row("Tags:", tags_edit_);
  add_row("Runner:", runner_row_widget);
  add_row("Data directory:", data_dir_edit_);
  add_row("Runner config:", runner_config_edit_);
  add_row("Environment:", env_edit_);
  layout->addLayout(form);
  layout->addWidget(last_error_label_);

  // Without a stretch factor here, Qt spreads the QScrollArea's leftover
  // height evenly across every form row instead of leaving one gap below.
  layout->addStretch(1);

  // After the stretch, not right below the form: pushed to the very bottom
  // of the page, next to the containing page's own Save/Cancel, rather than
  // sitting in the middle of the form fields above it.
  auto* advanced_button = new QPushButton("Advanced settings…", this);
  // QPushButton defaults to Fixed horizontal — same sidebar-floor bug as
  // install_path_label_ above, this time spilling the button's own text.
  advanced_button->setSizePolicy(QSizePolicy::Ignored, advanced_button->sizePolicy().verticalPolicy());
  advanced_button->setToolTip("Per-game overrides of the global settings.");
  connect(advanced_button, &QPushButton::clicked, this, &GameEditForm::OpenAdvanced);
  layout->addWidget(advanced_button);

  // Built now, shown later: overrides_->Load() (in Load(), below) needs
  // somewhere to live before the dialog's ever opened. Its own window, not
  // inline — one row per overridable key would cramp the sidebar.
  advanced_dialog_ = new QDialog(this);
  advanced_dialog_->setWindowTitle("Advanced settings");
  // Scaled off the real top-level window rather than a fixed size, so it's
  // never cramped on a small screen or tiny on a large one.
  advanced_dialog_->setMinimumSize(720, 480);
  advanced_dialog_->resize(qMax(720, window()->width() * 3 / 5),
                           qMax(480, window()->height() * 3 / 4));
  auto* dialog_layout = new QVBoxLayout(advanced_dialog_);

  overrides_ = new mira_gui::OverridesEditor(id_, advanced_dialog_);
  dialog_layout->addWidget(overrides_, /*stretch=*/1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, advanced_dialog_);
  connect(buttons, &QDialogButtonBox::rejected, advanced_dialog_, &QDialog::hide);
  connect(buttons, &QDialogButtonBox::accepted, advanced_dialog_, &QDialog::hide);
  dialog_layout->addWidget(buttons);

  setEnabled(false);
  Load();
}

void GameEditForm::SetArtworkStore(ArtworkStore* store) { hero_art_->SetArtworkStore(store); }

void GameEditForm::OpenAdvanced() {
  advanced_dialog_->show();
  advanced_dialog_->raise();
  advanced_dialog_->activateWindow();
}

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

  // GameDetail, not GameSummary: hero_art_ only needs the handful of fields
  // the two share, and GetGameAsync (Load(), above) is what this form has.
  mira_gui::GameSummary summary;
  summary.id = game.id;
  summary.name = game.name;
  summary.status = game.status;
  summary.platform = game.platform;
  summary.runner_ref = game.runner_ref;
  summary.last_error = game.last_error;
  summary.install_path = game.install_path;
  summary.reviewed = game.reviewed;
  summary.confidence = game.confidence;
  summary.last_played_at = game.last_played_at;
  summary.play_seconds = game.play_seconds;
  summary.tags = game.tags;
  hero_art_->ShowGame(summary);

  // "Ready" is the common case and says nothing worth a line of its own —
  // only a state that needs attention earns one.
  form_->setRowVisible(status_label_, game.status != "ready");
  status_label_->setText(QString::fromStdString(game.status));
  theme::SetStyleProperty(status_label_, "status", QString::fromStdString(game.status));

  install_path_ = game.install_path;
  install_path_label_->setText(QString::fromStdString(game.install_path));
  install_path_label_->setToolTip(install_path_label_->text());

  if (game.source == "steam") {
    source_note_label_->setText(
        "Imported from Steam. Steam launches this game itself, using its own record of the "
        "executable — the Executable field below isn't what runs it, and editing it won't "
        "change how it launches.");
  } else if (game.source == "lutris") {
    source_note_label_->setText(
        "Imported from Lutris. The executable below came from Lutris's own config, not from "
        "scanning the install folder, so there's no list of alternates to pick from here.");
  }
  form_->setRowVisible(source_note_label_, game.source == "steam" || game.source == "lutris");

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
