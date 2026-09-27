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

#include "../client/MiradClient.h"
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
  footer->addStretch(1);
  auto* all = new QPushButton("All settings…", this);
  all->setFlat(true);
  all->setCursor(Qt::PointingHandCursor);
  connect(all, &QPushButton::clicked, this, [this] {
    emit OpenSettingsRequested(settings_.empty() ? QString() : QString::fromStdString(settings_.front().entry.key));
  });
  footer->addWidget(all);
  layout->addLayout(footer);

  LoadSettings();
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
  connect(runner_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
    const QString ref = runner_->itemData(index).toString();
    if (ref != runner_ref_) SetRunner(ref, /*apply_to_games=*/false);
  });
  column_layout->addWidget(runner_);

  auto* note_row = new QHBoxLayout();
  runner_note_ = new QLabel(column);
  runner_note_->setWordWrap(true);
  runner_note_->setProperty("role", "muted");
  note_row->addWidget(runner_note_, /*stretch=*/1);
  runner_apply_ = new QPushButton(column);
  runner_apply_->setVisible(false);
  connect(runner_apply_, &QPushButton::clicked, this, [this] { SetRunner(runner_ref_, /*apply_to_games=*/true); });
  note_row->addWidget(runner_apply_, 0, Qt::AlignTop);
  column_layout->addLayout(note_row);

  const QString runner_doc =
      source_.kind == SourceInfo::Kind::Launcher
          ? "The Wine or Proton build " + source_.name + " and its games run with."
          : "The Wine or Proton build for " + source_.name + " games that have none of their own.";
  form->addRow(LabelWithHelp("Runner", runner_doc, this), column);
  LoadRunner();
}

void SourceSettingsCard::LoadRunner() {
  if (runner_ == nullptr) return;
  MiradClient::GetSourceRunnerAsync(this, id_, [this](SourceRunnerResult runner) { ShowRunner(runner); });
}

void SourceSettingsCard::ShowRunner(const SourceRunnerResult& runner) {
  runner_apply_->setVisible(false);
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
    int current = runner_->findData(runner_ref_);
    if (current < 0) {
      runner_->addItem(runner_ref_ + " (not installed)", runner_ref_);
      current = runner_->count() - 1;
    }
    runner_->setCurrentIndex(current);
    runner_->setEnabled(true);
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
    runner_apply_->setVisible(true);
  }
}

void SourceSettingsCard::SetRunner(const QString& runner_ref, bool apply_to_games) {
  runner_->setEnabled(false);
  runner_apply_->setEnabled(false);
  MiradClient::SetSourceRunnerAsync(
      this, id_, runner_ref.toStdString(), apply_to_games, [this](SourceRunnerResult result) {
        runner_apply_->setEnabled(true);
        if (!result.ok) {
          ShowStatus("Could not change the runner: " + QString::fromStdString(result.error), true);
          LoadRunner();
          return;
        }
        ShowStatus("Saved", false);
        ShowRunner(result);
      });
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
        if (editor.check != nullptr) connect(editor.check, &QCheckBox::toggled, this, [this, i] { Commit(i); });
        if (editor.spin != nullptr) {
          connect(editor.spin, &QDoubleSpinBox::valueChanged, this, [this, i] { Commit(i); });
        }
        if (editor.combo != nullptr) {
          connect(editor.combo, QOverload<int>::of(&QComboBox::activated), this, [this, i] { Commit(i); });
          if (editor.combo->lineEdit() != nullptr) {
            connect(editor.combo->lineEdit(), &QLineEdit::editingFinished, this, [this, i] { Commit(i); });
          }
        }
        if (editor.line != nullptr) connect(editor.line, &QLineEdit::editingFinished, this, [this, i] { Commit(i); });
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
      loading_ = true;
      for (SettingEditor& editor : settings_) {
        const auto it = config.values.find(editor.entry.key);
        editor.SetText(it != config.values.end() ? it->second : editor.entry.default_display);
        editor.original = editor.Text();
      }
      loading_ = false;
    });
  });
}

void SourceSettingsCard::Commit(size_t index) {
  if (loading_) return;
  SettingEditor& editor = settings_[index];
  const std::string value = editor.Text();
  if (value == editor.original) return;
  MiradClient::PatchConfigAsync(this, {ConfigEdit{editor.entry.key, editor.entry.type, value}},
                                [this, index, value](PatchConfigResult result) {
                                  SettingEditor& done = settings_[index];
                                  if (!result.ok) {
                                    loading_ = true;
                                    done.SetText(done.original);
                                    loading_ = false;
                                    ShowStatus(QString::fromStdString(result.error), true);
                                    return;
                                  }
                                  done.original = value;
                                  ShowStatus("Saved", false);
                                });
}

void SourceSettingsCard::ResetSetting(size_t index) {
  MiradClient::ResetConfigKeyAsync(this, settings_[index].entry.key, [this, index](PatchConfigResult result) {
    if (!result.ok) {
      ShowStatus(QString::fromStdString(result.error), true);
      return;
    }
    SettingEditor& editor = settings_[index];
    loading_ = true;
    editor.SetText(editor.entry.default_display);
    loading_ = false;
    editor.original = editor.Text();
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
