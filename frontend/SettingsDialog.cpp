#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <map>

namespace {
// The one schema key GET /v1/config/schema can't itself mark as "this is a
// runner reference" (docs/config schema has no such concept) — see the
// class comment in SettingsDialog.h for why it gets a picker (GET
// /v1/runners) instead of a plain text box like everything else here.
// "default_runner.native" is deliberately NOT included: its schema
// validator (config::RunnerRef in Schema.cpp) requires a literal
// "kind:name" and rejects "auto", and there is exactly one native runner
// anyway (RunnerRegistry has nothing to pick "best" between for it) — so a
// picker would only ever offer an option ("Auto") guaranteed to fail
// validation.
constexpr std::array<const char*, 1> kRunnerKeys = {"default_runner.windows"};

bool IsRunnerKey(const std::string& key) {
  return std::ranges::find(kRunnerKeys, key) != kRunnerKeys.end();
}

// Groups rows for readability — see the class comment in SettingsDialog.h.
// Everything currently in src/config/Schema.cpp is named explicitly; a
// schema key added later without a matching entry here still lands
// somewhere sensible via the dotted-prefix guess below, never disappears.
QString CategoryFor(const std::string& key) {
  static const std::map<std::string, QString> kOverrides = {
      {"library_roots", "Library"},        {"prefix_root", "Library"},
      {"prefix_provider", "Library"},      {"prefix_template", "Library"},
      {"auto_setup", "Library"},           {"open_config_on_add", "Library"},
      {"command_wrappers", "Library"},     {"default_runner.windows", "Runners"},
      {"default_runner.native", "Runners"}, {"runner_search_paths", "Runners"},
      {"wine_search_paths", "Runners"},    {"socket_path", "Advanced"},
      {"log.level", "Advanced"},           {"events.sse_keepalive_s", "Advanced"},
  };
  if (const auto it = kOverrides.find(key); it != kOverrides.end()) return it->second;

  const size_t dot = key.find('.');
  if (dot == std::string::npos) return "General";
  const std::string prefix = key.substr(0, dot);
  if (prefix == "detect") return "Detection";
  if (prefix == "scan") return "Scanning";
  if (prefix == "default_runner") return "Runners";
  if (prefix == "log" || prefix == "events") return "Advanced";

  QString label = QString::fromStdString(prefix).replace('_', ' ');
  if (!label.isEmpty()) label[0] = label[0].toUpper();
  return label;
}

// Categories appear in this order when present; anything else (a future,
// unmapped prefix) is appended alphabetically after — see CategoryFor.
const QStringList& CategoryOrder() {
  static const QStringList order = {"Library", "Runners", "Detection", "Scanning", "Advanced",
                                     "General"};
  return order;
}
}  // namespace

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

void SettingsDialog::Load() {
  mira_gui::MiradClient::GetConfigSchemaAsync(this, [this](mira_gui::ConfigSchemaResult schema) {
    if (!schema.ok) {
      QMessageBox::warning(this, "Failed to load settings", QString::fromStdString(schema.error));
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
        QMessageBox::warning(this, "Failed to load settings", QString::fromStdString(config.error));
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
  for (size_t i = 0; i < fields_.size(); ++i) buckets[CategoryFor(fields_[i].entry.key)].push_back(i);

  QStringList ordered_categories;
  for (const QString& category : CategoryOrder()) {
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
      } else if (IsRunnerKey(field.entry.key)) {
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
          QMessageBox::warning(this, "Reset failed", QString::fromStdString(result.error));
          return;
        }
        Field& field = fields_[index];
        SetFieldText(field, field.entry.default_display);
        field.original = CurrentText(field);
      });
}

void SettingsDialog::Save() {
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
      QMessageBox::warning(this, "Save failed", QString::fromStdString(result.error));
      return;
    }
    accept();
  });
}
