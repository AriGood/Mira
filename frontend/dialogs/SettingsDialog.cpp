#include "SettingsDialog.h"

#include "SettingsCategories.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>

#include "../ui/Notify.h"
#include "../ui/SystemNotifier.h"
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <map>


SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Settings");
  resize(640, 620);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(10);

  show_advanced_ = new QCheckBox("Show advanced && expert settings", this);
  connect(show_advanced_, &QCheckBox::toggled, this, &SettingsDialog::SetAdvancedVisible);
  layout->addWidget(show_advanced_);

  auto* groups_container = new QWidget(this);
  groups_layout_ = new QVBoxLayout(groups_container);
  groups_layout_->setContentsMargins(2, 2, 2, 2);
  // After groups_layout_ exists, not before — this adds a widget to it.
  BuildInterfaceGroup();
  groups_layout_->setSpacing(14);

  auto* scroll = new QScrollArea(this);
  scroll->setWidget(groups_container);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  layout->addWidget(scroll, /*stretch=*/1);

  auto* buttons = new QDialogButtonBox(this);
  save_button_ = buttons->addButton("Save", QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Close);
  connect(save_button_, &QPushButton::clicked, this, &SettingsDialog::Save);
  connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
  layout->addWidget(buttons);

  setEnabled(false);
  Load();
}

void SettingsDialog::BuildInterfaceGroup() {
  auto* box = new QGroupBox("Interface (this frontend only)", this);
  auto* form = new QFormLayout(box);
  form->setVerticalSpacing(8);
  form->setHorizontalSpacing(14);

  scan_on_startup_ = new QCheckBox(box);
  scan_on_startup_->setChecked(true);
  scan_on_startup_->setToolTip(
      "Run a library scan when the frontend opens. mirad's own watcher keeps the library current "
      "while it runs, so this only matters for changes made while it was stopped.");
  form->addRow("Scan the library on startup", scan_on_startup_);

  notifications_ = new QComboBox(box);
  notifications_->addItem("When Mira isn't focused", "auto");
  notifications_->addItem("Always as a desktop notification", "system");
  notifications_->addItem("Always inside the window", "in_app");
  notifications_->setToolTip(
      mira_gui::notify::system_notifier::Available()
          ? "Background results — a runner finishing downloading, metadata arriving — can go to "
            "the desktop's notification service instead of a card inside the window. A desktop "
            "notification survives Mira being minimised and lands in the shell's notification "
            "history."
          : "No desktop notification service is running, so everything is shown inside the "
            "window whatever this says.");
  form->addRow("Show background results", notifications_);

  auto* note = new QLabel(
      "Stored in frontend.toml, beside settings.toml — the daemon keeps it verbatim and never "
      "interprets it. Everything below is a backend setting.",
      box);
  note->setWordWrap(true);
  note->setStyleSheet("color: #9e9e9e; font-size: 11px;");
  form->addRow(note);

  groups_layout_->addWidget(box);
  LoadFrontendPrefs();
}

void SettingsDialog::LoadFrontendPrefs() {
  // Seeded from what the process is already using, so the row is correct
  // even before (or without) the round trip below.
  notifications_original_ =
      mira_gui::notify::DeliveryToString(mira_gui::notify::CurrentDelivery());
  notifications_->setCurrentIndex(notifications_->findData(notifications_original_));

  mira_gui::MiradClient::GetFrontendPrefsAsync(this, [this](mira_gui::FrontendPrefsResult result) {
    if (!result.ok) return;  // the defaults are already shown
    if (result.prefs.scan_on_startup) {
      scan_on_startup_original_ = *result.prefs.scan_on_startup;
      scan_on_startup_->setChecked(scan_on_startup_original_);
    }
    if (result.prefs.notifications) {
      notifications_original_ = QString::fromStdString(*result.prefs.notifications);
      const int index = notifications_->findData(notifications_original_);
      if (index >= 0) notifications_->setCurrentIndex(index);
    }
  });
}

void SettingsDialog::Load() {
  mira_gui::MiradClient::GetConfigSchemaAsync(this, [this](mira_gui::ConfigSchemaResult schema) {
    if (!schema.ok) {
      mira_gui::notify::Failed(this, "Could not load the settings schema.",
                               QString::fromStdString(schema.error));
      reject();
      return;
    }

    for (mira_gui::ConfigSchemaEntry& entry : schema.entries) {
      fields_.push_back(Field{std::move(entry), std::string(), nullptr, nullptr, nullptr, nullptr, nullptr});
    }
    BuildRows();

    // Independent of the config fetch below — populating a runner combo's
    // item list never touches its current text (see PopulateRunnerCombos),
    // so this can land before or after the field values load with the same
    // result, same as GameDetailDialog's identical pattern.
    mira_gui::MiradClient::ListRunnersAsync(this, [this](mira_gui::RunnersResult result) {
      PopulateRunnerCombos(result);
    });

    mira_gui::MiradClient::GetConfigAsync(this, [this](mira_gui::ConfigResult config) {
      if (!config.ok) {
        mira_gui::notify::Failed(this, "Could not load the current settings.",
                                 QString::fromStdString(config.error));
        reject();
        return;
      }
      for (Field& field : fields_) {
        const auto it = config.values.find(field.entry.key);
        SetFieldText(field, it != config.values.end() ? it->second : field.entry.default_display);
        field.original = CurrentText(field);
      }
      setEnabled(true);
    });
  });
}

void SettingsDialog::BuildRows() {
  // Bucket field indices by category, preserving each field's original
  // (schema declaration) order within its bucket.
  std::map<QString, std::vector<size_t>> buckets;
  for (size_t i = 0; i < fields_.size(); ++i) buckets[mira_gui::settings::CategoryFor(fields_[i].entry.key)].push_back(i);

  QStringList ordered_categories;
  for (const QString& category : mira_gui::settings::CategoryOrder()) {
    if (buckets.contains(category)) ordered_categories.push_back(category);
  }
  for (const auto& [category, indices] : buckets) {
    if (!ordered_categories.contains(category)) ordered_categories.push_back(category);
  }

  for (const QString& category : ordered_categories) {
    CategoryGroup group;
    group.box = new QGroupBox(category, this);
    group.form = new QFormLayout();
    group.form->setVerticalSpacing(10);
    group.form->setHorizontalSpacing(14);
    group.form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    group.box->setLayout(group.form);

    for (const size_t i : buckets[category]) {
      Field& field = fields_[i];
      field.owner_form = group.form;
      if (field.entry.tier == "basic") group.has_basic = true;

      auto* row_widget = new QWidget(this);
      auto* row_layout = new QHBoxLayout(row_widget);
      row_layout->setContentsMargins(0, 0, 0, 0);
      row_layout->setSpacing(8);

      if (field.entry.type == "a boolean") {
        // No reset button here: a checkbox only has two states, so its
        // default is always exactly one click away — a dedicated button
        // would be redundant chrome for what a line edit's free-form text
        // actually needs a button for.
        field.check = new QCheckBox(row_widget);
        row_layout->addWidget(field.check);
        row_layout->addStretch(1);
      } else if (mira_gui::settings::IsRunnerKey(field.entry.key)) {
        // Same editable-combo pattern as GameDetailDialog's runner field:
        // typeable (for a runner GET /v1/runners won't list, e.g. "native:
        // native") or pickable by name from what's actually installed.
        // Populated once GET /v1/runners resolves — see PopulateRunnerCombos.
        field.combo = new QComboBox(row_widget);
        field.combo->setEditable(true);
        field.combo->setInsertPolicy(QComboBox::NoInsert);
        connect(field.combo, QOverload<int>::of(&QComboBox::activated), this,
                [field = &field](int idx) {
                  field->combo->setEditText(field->combo->itemData(idx).toString());
                  field->combo->lineEdit()->setCursorPosition(0);
                });
        row_layout->addWidget(field.combo, /*stretch=*/1);

        auto* reset_button = new QPushButton("Reset", row_widget);
        reset_button->setMaximumWidth(56);
        reset_button->setToolTip(
            QString("Reset to default: %1").arg(QString::fromStdString(field.entry.default_display)));
        connect(reset_button, &QPushButton::clicked, this, [this, i] { ResetField(i); });
        row_layout->addWidget(reset_button);
      } else {
        field.line = new QLineEdit(row_widget);
        if (field.entry.type == "an array of strings") {
          field.line->setPlaceholderText("comma-separated");
        }
        if (mira_gui::settings::IsSecretKey(field.entry.key)) {
          // PasswordEchoOnEdit, not Password: the value has to be checkable
          // against what the site shows, and a key you can never read back
          // is a key you re-paste every time you doubt it.
          field.line->setEchoMode(QLineEdit::PasswordEchoOnEdit);
        }
        row_layout->addWidget(field.line, /*stretch=*/1);

        auto* reset_button = new QPushButton("Reset", row_widget);
        reset_button->setMaximumWidth(56);
        reset_button->setToolTip(
            QString("Reset to default: %1").arg(QString::fromStdString(field.entry.default_display)));
        connect(reset_button, &QPushButton::clicked, this, [this, i] { ResetField(i); });
        row_layout->addWidget(reset_button);
      }

      auto* label = new QLabel(QString::fromStdString(field.entry.key), this);
      label->setToolTip(QString::fromStdString(field.entry.doc));
      row_widget->setToolTip(QString::fromStdString(field.entry.doc));

      field.row_widget = row_widget;
      group.form->addRow(label, row_widget);
    }

    groups_layout_->addWidget(group.box);
    groups_.push_back(group);
  }
  groups_layout_->addStretch(1);

  SetAdvancedVisible(show_advanced_->isChecked());
}

void SettingsDialog::PopulateRunnerCombos(const mira_gui::RunnersResult& result) {
  for (Field& field : fields_) {
    if (!field.combo) continue;

    // clear() + addItem() resets the displayed text to the first item added
    // — that happens as part of the widget's own state, not a signal, so
    // blockSignals() below doesn't prevent it. GetConfigAsync's SetFieldText
    // and this can resolve in either order (see the comment in Load()), so
    // whatever's already showing has to survive a repopulate regardless of
    // which one got here first.
    const QString current = field.combo->currentText();
    field.combo->blockSignals(true);
    field.combo->clear();
    // "auto" is default_runner.windows's own literal default (resolved to
    // the best installed runner at provision time — RunnerRegistry.cpp),
    // not a stand-in for an empty value the way a per-game runner_ref's
    // empty string is, so it's a real, always-available item here rather
    // than a placeholder.
    field.combo->addItem("Auto (best available)", "auto");
    if (result.ok) {
      for (const mira_gui::RunnerInfo& runner : result.runners) {
        const QString label = QString("%1 (%2)").arg(QString::fromStdString(runner.name),
                                                       QString::fromStdString(runner.kind));
        field.combo->addItem(label, QString::fromStdString(runner.reference));
      }
    }
    field.combo->setEditText(current);
    field.combo->blockSignals(false);
  }
}

void SettingsDialog::SetAdvancedVisible(bool show) {
  for (const Field& field : fields_) {
    if (field.entry.tier != "basic") field.owner_form->setRowVisible(field.row_widget, show);
  }
  for (const CategoryGroup& group : groups_) group.box->setVisible(group.has_basic || show);
}

std::string SettingsDialog::CurrentText(const Field& field) const {
  if (field.check) return field.check->isChecked() ? "true" : "false";
  if (field.combo) return field.combo->currentText().toStdString();
  return field.line->text().toStdString();
}

void SettingsDialog::SetFieldText(Field& field, const std::string& text) {
  if (field.check) {
    field.check->setChecked(text == "true");
    return;
  }
  QLineEdit* edit = field.combo ? field.combo->lineEdit() : field.line;
  edit->setText(QString::fromStdString(text));
  // setText() otherwise leaves the cursor at the end, scrolling a value
  // longer than the field to show its tail — the start of the path/list
  // is what identifies it, so that's what should be visible first. The
  // field still scrolls normally once clicked into.
  edit->setCursorPosition(0);
}

void SettingsDialog::ResetField(size_t index) {
  mira_gui::MiradClient::ResetConfigKeyAsync(
      this, fields_[index].entry.key, [this, index](mira_gui::PatchConfigResult result) {
        if (!result.ok) {
          mira_gui::notify::Failed(this, "Could not reset that setting.",
                                   QString::fromStdString(result.error));
          return;
        }
        Field& field = fields_[index];
        SetFieldText(field, field.entry.default_display);
        field.original = CurrentText(field);
      });
}

void SettingsDialog::Save() {
  // Sent separately from the schema edits below, because it is a different
  // file behind a different key — and unconditionally skipped when
  // unchanged, so opening and saving this dialog never rewrites
  // frontend.toml for nothing.
  const QString notifications = notifications_->currentData().toString();
  if (scan_on_startup_->isChecked() != scan_on_startup_original_ ||
      notifications != notifications_original_) {
    mira_gui::FrontendPrefs prefs;
    prefs.scan_on_startup = scan_on_startup_->isChecked();
    prefs.notifications = notifications.toStdString();
    scan_on_startup_original_ = *prefs.scan_on_startup;
    notifications_original_ = notifications;
    // Applied to the running process as well as saved: the next toast
    // should obey the row that was just changed, not wait for a restart.
    mira_gui::notify::SetDelivery(mira_gui::notify::DeliveryFromString(notifications));
    mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
  }

  std::vector<mira_gui::ConfigEdit> edits;
  for (const Field& field : fields_) {
    const std::string current = CurrentText(field);
    if (current != field.original) {
      edits.push_back(mira_gui::ConfigEdit{field.entry.key, field.entry.type, current});
    }
  }

  if (edits.empty()) {
    accept();
    return;
  }

  setEnabled(false);
  mira_gui::MiradClient::PatchConfigAsync(this, edits, [this](mira_gui::PatchConfigResult result) {
    setEnabled(true);
    if (!result.ok) {
      mira_gui::notify::Failed(this, "Could not save the settings.",
                               QString::fromStdString(result.error));
      return;
    }
    accept();
  });
}
