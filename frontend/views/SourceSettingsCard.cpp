#include "SourceSettingsCard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

#include "../client/MiradClient.h"
#include "../ui/ErrorHelp.h"
#include "../ui/HelpButton.h"
#include "../ui/Theme.h"

namespace mira_gui {
namespace {

bool IsSourceKey(const SourceInfo& source, const std::string& key) {
  const std::string id = source.id.toStdString();
  if (source.kind == SourceInfo::Kind::Launcher && key.starts_with("launchers." + id + ".")) return true;
  // enabled belongs to Manage sources, runner has its own row, collections their own dialog.
  return key.starts_with(id + ".") && key != id + ".enabled" && key != id + ".runner" &&
         key != "itch.collections";
}

QString Games(int count) { return QString("%1 game%2").arg(count).arg(count == 1 ? "" : "s"); }

}  // namespace

SourceSettingsCard::SourceSettingsCard(const SourceInfo& source, QWidget* parent)
    : QFrame(parent), source_(source), id_(source.id.toStdString()) {
  setObjectName("source_settings");
  const theme::Tokens& tokens = theme::Current();
  setStyleSheet(QString("QFrame#source_settings { background: %1; border: 1px solid %2; border-radius: %3px; }")
                    .arg(tokens.surface.name(), tokens.border.name())
                    .arg(tokens.radius_panel));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(18, 14, 18, 14);
  layout->setSpacing(8);

  auto* header = new QHBoxLayout();
  auto* title = new QLabel(source_.name + " settings", this);
  title->setProperty("role", "section");
  header->addWidget(title);
  header->addStretch(1);
  status_ = new QLabel(this);
  status_->setVisible(false);
  header->addWidget(status_);
  layout->addLayout(header);

  form_ = new QFormLayout();
  form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  layout->addLayout(form_);
  if (HasRunner()) BuildRunnerRow(form_);

  auto* footer = new QHBoxLayout();
  auto* all = new QPushButton("All settings…", this);
  all->setFlat(true);
  all->setCursor(Qt::PointingHandCursor);
  connect(all, &QPushButton::clicked, this, [this] {
    emit OpenSettingsRequested(settings_.empty() ? QString() : QString::fromStdString(settings_.front().entry.key));
  });
  footer->addWidget(all);
  footer->addStretch(1);
  // Same pair, and names, as the settings screen's.
  discard_ = new QPushButton("Discard", this);
  discard_->setToolTip("Discard unsaved changes and go back to the last saved settings.");
  connect(discard_, &QPushButton::clicked, this, &SourceSettingsCard::Discard);
  footer->addWidget(discard_);
  save_ = new QPushButton("Save", this);
  connect(save_, &QPushButton::clicked, this, &SourceSettingsCard::Save);
  footer->addWidget(save_);
  layout->addLayout(footer);

  UpdateButtons();
  LoadSettings();
}

bool SourceSettingsCard::RunnerDirty() const {
  return runner_ != nullptr && runner_->isEnabled() && runner_->currentData().toString() != runner_ref_;
}

bool SourceSettingsCard::IsDirty() const {
  if (RunnerDirty()) return true;
  return std::ranges::any_of(settings_, [](const SettingEditor& editor) { return editor.Text() != editor.original; });
}

void SourceSettingsCard::UpdateButtons() {
  const bool dirty = IsDirty();
  discard_->setEnabled(dirty && !saving_);
  save_->setEnabled(dirty && !saving_);
  // Switching every game to a runner that isn't saved yet would be a surprise.
  if (runner_apply_ != nullptr) runner_apply_->setVisible(runner_can_apply_ && !RunnerDirty());
}

void SourceSettingsCard::Discard() {
  if (runner_ != nullptr) SelectRunner(runner_ref_);
  for (SettingEditor& editor : settings_) editor.SetText(editor.original);
  status_->setVisible(false);
  UpdateButtons();
}

void SourceSettingsCard::Save() {
  if (saving_ || !IsDirty()) {
    emit SaveFinished(true);
    return;
  }
  std::vector<ConfigEdit> edits;
  std::vector<std::pair<size_t, std::string>> saved;
  for (size_t i = 0; i < settings_.size(); ++i) {
    const std::string value = settings_[i].Text();
    if (value == settings_[i].original) continue;
    edits.push_back({settings_[i].entry.key, settings_[i].entry.type, value});
    saved.emplace_back(i, value);
  }
  const bool runner_changed = RunnerDirty();
  const QString runner_ref = runner_changed ? runner_->currentData().toString() : QString();
  saving_ = true;
  UpdateButtons();

  // Settings first, as one validated patch, then the runner; either failing stops there.
  auto save_runner = [this, runner_changed, runner_ref] {
    if (!runner_changed) {
      saving_ = false;
      ShowStatus("Saved", false);
      UpdateButtons();
      emit SaveFinished(true);
      return;
    }
    MiradClient::SetSourceRunnerAsync(this, id_, runner_ref.toStdString(), /*apply_to_games=*/false,
                                      [this](SourceRunnerResult result) {
                                        saving_ = false;
                                        if (!result.ok) {
                                          ShowStatus("Could not change the runner: " + error_help::Describe(result.error), true);
                                          UpdateButtons();
                                          emit SaveFinished(false);
                                          return;
                                        }
                                        ShowRunner(result);
                                        ShowStatus("Saved", false);
                                        UpdateButtons();
                                        emit SaveFinished(true);
                                      });
  };
  if (edits.empty()) {
    save_runner();
    return;
  }
  MiradClient::PatchConfigAsync(this, edits, [this, saved, save_runner](PatchConfigResult result) {
    if (!result.ok) {
      saving_ = false;
      ShowStatus(error_help::Describe(result.error), true);
      UpdateButtons();
      emit SaveFinished(false);
      return;
    }
    for (const auto& [index, value] : saved) settings_[index].original = value;
    save_runner();
  });
}

bool SourceSettingsCard::HasRunner() const {
  return source_.kind == SourceInfo::Kind::Launcher ||
         (source_.kind == SourceInfo::Kind::Store && id_ != "humble");
}

void SourceSettingsCard::Refresh() {
  LoadRunner();
  LoadSettings();
}

void SourceSettingsCard::BuildRunnerRow(QFormLayout* form) {
  auto* column = new QWidget(this);
  auto* column_layout = new QVBoxLayout(column);
  column_layout->setContentsMargins(0, 0, 0, 0);
  column_layout->setSpacing(4);

  runner_ = new QComboBox(column);
  runner_->setEnabled(false);
  connect(runner_, QOverload<int>::of(&QComboBox::activated), this, &SourceSettingsCard::UpdateButtons);
  column_layout->addWidget(runner_);

  auto* note_row = new QHBoxLayout();
  runner_note_ = new QLabel(column);
  runner_note_->setWordWrap(true);
  runner_note_->setProperty("role", "muted");
  note_row->addWidget(runner_note_, /*stretch=*/1);
  runner_apply_ = new QPushButton(column);
  runner_apply_->setVisible(false);
  connect(runner_apply_, &QPushButton::clicked, this, [this] {
    runner_apply_->setEnabled(false);
    MiradClient::SetSourceRunnerAsync(this, id_, runner_ref_.toStdString(), /*apply_to_games=*/true,
                                      [this](SourceRunnerResult result) {
                                        runner_apply_->setEnabled(true);
                                        if (!result.ok) {
                                          ShowStatus("Could not switch the games: " + error_help::Describe(result.error), true);
                                          return;
                                        }
                                        ShowStatus("Switched", false);
                                        ShowRunner(result);
                                      });
  });
  note_row->addWidget(runner_apply_, 0, Qt::AlignTop);
  column_layout->addLayout(note_row);

  const QString runner_doc =
      source_.kind == SourceInfo::Kind::Launcher
          ? "The Wine or Proton build that " + source_.name + " and its games run with."
          : "The Wine or Proton build for " + source_.name + " games that have no runner of their own.";
  form->addRow(LabelWithHelp("Runner", runner_doc, this), column);
  LoadRunner();
}

void SourceSettingsCard::LoadRunner() {
  if (runner_ == nullptr) return;
  MiradClient::GetSourceRunnerAsync(this, id_, [this](SourceRunnerResult runner) { ShowRunner(runner); });
}

void SourceSettingsCard::SelectRunner(const QString& runner_ref) {
  int index = runner_->findData(runner_ref);
  if (index < 0) {
    runner_->addItem(runner_ref + " (not installed)", runner_ref);
    index = runner_->count() - 1;
  }
  runner_->setCurrentIndex(index);
}

void SourceSettingsCard::ShowRunner(const SourceRunnerResult& runner) {
  runner_can_apply_ = false;
  UpdateButtons();
  if (!runner.ok) {
    runner_->setEnabled(false);
    runner_note_->setText(source_.kind == SourceInfo::Kind::Launcher
                              ? "Install " + source_.name + " to choose its runner."
                              : "Could not ask mirad: " + QString::fromStdString(runner.error));
    return;
  }
  runner_ref_ = QString::fromStdString(runner.runner_ref);
  MiradClient::ListRunnersAsync(this, [this](RunnersResult runners) {
    runner_->clear();
    runner_->addItem("Default (from Runners settings)", QString());
    runner_->addItem("Auto (best available)", QString("auto"));
    if (runners.ok) {
      for (const RunnerInfo& info : runners.runners) {
        if (info.kind != "wine" && info.kind != "proton") continue;
        const QString label = QString::fromStdString(info.label.empty() ? info.name : info.label);
        runner_->addItem(QString("%1 (%2)").arg(label, QString::fromStdString(info.kind)),
                         QString::fromStdString(info.reference));
      }
    }
    SelectRunner(runner_ref_);
    runner_->setEnabled(true);
    UpdateButtons();
  });

  if (source_.kind == SourceInfo::Kind::Launcher) {
    runner_note_->setText(runner.games == 0
                              ? source_.name + " and the games it installs share one prefix and runner."
                              : source_.name + " and its " + Games(runner.games) +
                                    " share one prefix, so they switch together.");
  } else if (runner.games == 0) {
    runner_note_->setText("For " + source_.name + " games you install.");
  } else if (runner.differing == 0) {
    runner_note_->setText("All " + Games(runner.games) + " from " + source_.name + " use it.");
  } else {
    runner_note_->setText(QString("For new games and ones without their own runner. %1 of %2 games %3 another one.")
                              .arg(runner.differing)
                              .arg(runner.games)
                              .arg(runner.differing == 1 ? "uses" : "use"));
    runner_apply_->setText(runner.differing == 1 ? "Switch it too" : "Switch them too");
    runner_can_apply_ = true;
    UpdateButtons();
  }
}

void SourceSettingsCard::LoadSettings() {
  MiradClient::GetConfigSchemaAsync(this, [this](ConfigSchemaResult schema) {
    if (!schema.ok) {
      ShowStatus("Could not load settings: " + QString::fromStdString(schema.error), true);
      return;
    }
    if (settings_.empty()) {
      for (ConfigSchemaEntry& entry : schema.entries) {
        if (!IsSourceKey(source_, entry.key)) continue;
        SettingEditor editor;
        editor.entry = std::move(entry);
        settings_.push_back(std::move(editor));
      }
      for (size_t i = 0; i < settings_.size(); ++i) {
        SettingEditor& editor = settings_[i];
        QWidget* row = editor.Build(this, [this, i] { ResetSetting(i); });
        if (editor.check != nullptr) connect(editor.check, &QCheckBox::toggled, this, &SourceSettingsCard::UpdateButtons);
        if (editor.spin != nullptr) {
          connect(editor.spin, &QDoubleSpinBox::valueChanged, this, &SourceSettingsCard::UpdateButtons);
        }
        if (editor.combo != nullptr) {
          connect(editor.combo, &QComboBox::currentTextChanged, this, &SourceSettingsCard::UpdateButtons);
        }
        if (editor.line != nullptr) connect(editor.line, &QLineEdit::textChanged, this, &SourceSettingsCard::UpdateButtons);
        form_->addRow(editor.BuildLabel(this), row);
      }
      MiradClient::ListRunnersAsync(this, [this](RunnersResult runners) {
        for (SettingEditor& editor : settings_) {
          if (editor.combo != nullptr && editor.entry.is_runner_ref) FillRunnerCombo(editor.combo, runners);
        }
      });
    }
    MiradClient::GetConfigAsync(this, [this](ConfigResult config) {
      if (!config.ok) {
        ShowStatus("Could not load settings: " + QString::fromStdString(config.error), true);
        return;
      }
      for (SettingEditor& editor : settings_) {
        const auto it = config.values.find(editor.entry.key);
        editor.SetText(it != config.values.end() ? it->second : editor.entry.default_display);
        editor.original = editor.Text();
      }
      UpdateButtons();
    });
  });
}

void SourceSettingsCard::ResetSetting(size_t index) {
  MiradClient::ResetConfigKeyAsync(this, settings_[index].entry.key, [this, index](PatchConfigResult result) {
    if (!result.ok) {
      ShowStatus(error_help::Describe(result.error), true);
      return;
    }
    SettingEditor& editor = settings_[index];
    editor.SetText(editor.entry.default_display);
    editor.original = editor.Text();
    UpdateButtons();
    ShowStatus("Reset", false);
  });
}

void SourceSettingsCard::ShowStatus(const QString& text, bool error) {
  status_->setProperty("role", error ? "error" : "muted");
  status_->style()->unpolish(status_);
  status_->style()->polish(status_);
  status_->setText(text);
  status_->setVisible(true);
}

}  // namespace mira_gui
