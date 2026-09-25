#include "GameEditForm.h"

#include "../client/JsonMapping.h"
#include "../dialogs/OverridesEditor.h"
#include "GamePresentation.h"
#include "HeroArtWidget.h"
#include "Theme.h"

#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <filesystem>

namespace mira_gui {

GameEditForm::GameEditForm(std::string id, QWidget* parent) : QWidget(parent), id_(std::move(id)) {
  // Two columns, not one: the art preview would otherwise scroll out of
  // view with the rest of the form.
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(16);

  art_column_ = new QWidget(this);
  QWidget* art_column = art_column_;
  art_column->setFixedWidth(220);
  auto* art_layout = new QVBoxLayout(art_column);
  art_layout->setContentsMargins(0, 0, 0, 0);
  art_layout->setSpacing(10);

  hero_art_ = new HeroArtWidget(art_column);
  art_layout->addWidget(hero_art_);
  art_layout->addStretch(1);
  layout->addWidget(art_column);

  auto* fields_column = new QWidget(this);
  auto* fields_layout = new QVBoxLayout(fields_column);
  fields_layout->setContentsMargins(0, 0, 0, 0);
  fields_layout->setSpacing(12);

  // Two columns with each label above its field: a label column to the left
  // spent a third of the width on words.
  auto* form = new QGridLayout();
  form->setVerticalSpacing(12);
  form->setHorizontalSpacing(20);
  form->setColumnStretch(0, 1);
  form->setColumnStretch(1, 1);
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

  // A caption above its field; the pair shows and hides together.
  auto add_field = [form, this](const QString& text, QWidget* field, int row, int column, int span) {
    auto* box = new QWidget(this);
    auto* box_layout = new QVBoxLayout(box);
    box_layout->setContentsMargins(0, 0, 0, 0);
    box_layout->setSpacing(5);
    auto* label = new QLabel(text, box);
    label->setProperty("role", "muted");
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    box_layout->addWidget(label);
    box_layout->addWidget(field);
    form->addWidget(box, row, column, 1, span);
    return box;
  };
  status_box_ = add_field("Status", status_label_, 0, 0, 2);
  add_field("Install path", install_path_label_, 1, 0, 2);
  form->addWidget(source_note_label_, 2, 0, 1, 2);
  add_field("Executable", exe_row_widget, 3, 0, 2);
  add_field("Arguments", args_edit_, 4, 0, 1);
  add_field("Working directory", working_dir_edit_, 4, 1, 1);
  add_field("Runner", runner_row_widget, 5, 0, 1);
  add_field("Data directory", data_dir_edit_, 5, 1, 1);
  add_field("Name", name_edit_, 6, 0, 1);
  add_field("Tags", tags_edit_, 6, 1, 1);
  add_field("Runner config", runner_config_edit_, 7, 0, 2);
  add_field("Environment", env_edit_, 8, 0, 2);
  fields_layout->addLayout(form);
  fields_layout->addWidget(last_error_label_);

  // Without a stretch factor here, Qt spreads the QScrollArea's leftover
  // height evenly across every form row instead of leaving one gap below.
  fields_layout->addStretch(1);

  // After the stretch, not right below the form: pushed to the very bottom
  // of the page, next to the containing page's own Save/Cancel, rather than
  // sitting in the middle of the form fields above it.
  advanced_button_ = new QPushButton("Advanced settings…", this);
  QPushButton* advanced_button = advanced_button_;
  // QPushButton defaults to Fixed horizontal — same sidebar-floor bug as
  // install_path_label_ above, this time spilling the button's own text.
  advanced_button->setSizePolicy(QSizePolicy::Ignored, advanced_button->sizePolicy().verticalPolicy());
  advanced_button->setToolTip("Per-game overrides of the global settings.");
  connect(advanced_button, &QPushButton::clicked, this, &GameEditForm::OpenAdvanced);
  fields_layout->addWidget(advanced_button);

  // Covers just the fields column when open — the art column stays on
  // screen either side of it, unlike the separate dialog this used to be.
  fields_stack_ = new QStackedWidget(this);
  fields_stack_->addWidget(fields_column);

  auto* advanced_page = new QWidget(this);
  auto* advanced_layout = new QVBoxLayout(advanced_page);
  advanced_layout->setContentsMargins(0, 0, 0, 0);
  advanced_layout->setSpacing(10);
  auto* advanced_back = new QPushButton("← Back", advanced_page);
  connect(advanced_back, &QPushButton::clicked, this, [this] {
    fields_stack_->setCurrentIndex(0);
    ResetScroll();
  });
  advanced_layout->addWidget(advanced_back, /*stretch=*/0, Qt::AlignLeft);
  overrides_ = new mira_gui::OverridesEditor(id_, advanced_page);
  advanced_layout->addWidget(overrides_, /*stretch=*/1);
  fields_stack_->addWidget(advanced_page);

  layout->addWidget(fields_stack_, /*stretch=*/1);

  setEnabled(false);
  Load();
}

void GameEditForm::SetArtworkStore(ArtworkStore* store) { hero_art_->SetArtworkStore(store); }

void GameEditForm::SetArtColumnVisible(bool visible) { art_column_->setVisible(visible); }

void GameEditForm::SetAdvancedButtonVisible(bool visible) { advanced_button_->setVisible(visible); }

void GameEditForm::RefreshCover() { hero_art_->RefreshCover(); }
void GameEditForm::RefreshBanner(const std::string& id) { hero_art_->RefreshBanner(id); }

void GameEditForm::OpenAdvanced() {
  fields_stack_->setCurrentIndex(1);
  ResetScroll();
}

void GameEditForm::ResetScroll() {
  // Both host contexts (LibraryWindow's overlay card, GameDetailDialog)
  // wrap this form in a QScrollArea it has no direct handle to — walking up
  // to find it beats each host remembering to reset scroll on page-switch.
  for (QWidget* ancestor = parentWidget(); ancestor != nullptr; ancestor = ancestor->parentWidget()) {
    if (auto* scroll_area = qobject_cast<QScrollArea*>(ancestor)) {
      scroll_area->verticalScrollBar()->setValue(0);
      return;
    }
  }
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
  status_box_->setVisible(game.status != "ready");
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
  source_note_label_->setVisible(game.source == "steam" || game.source == "lutris");

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
