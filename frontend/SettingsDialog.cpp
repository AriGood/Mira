#include "SettingsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Settings");
  resize(600, 560);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(10);

  show_advanced_ = new QCheckBox("Show advanced && expert settings", this);
  connect(show_advanced_, &QCheckBox::toggled, this, &SettingsDialog::SetAdvancedVisible);
  layout->addWidget(show_advanced_);

  auto* form_container = new QWidget(this);
  form_ = new QFormLayout(form_container);
  form_->setVerticalSpacing(10);
  form_->setHorizontalSpacing(14);
  form_->setRowWrapPolicy(QFormLayout::WrapLongRows);

  auto* scroll = new QScrollArea(this);
  scroll->setWidget(form_container);
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
      fields_.push_back(Field{std::move(entry), std::string(), nullptr, nullptr, nullptr});
    }
    BuildRows();

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
  for (size_t i = 0; i < fields_.size(); ++i) {
    Field& field = fields_[i];

    auto* row_widget = new QWidget(this);
    auto* row_layout = new QHBoxLayout(row_widget);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(8);

    if (field.entry.type == "a boolean") {
      field.check = new QCheckBox(row_widget);
      row_layout->addWidget(field.check);
    } else {
      field.line = new QLineEdit(row_widget);
      if (field.entry.type == "an array of strings") {
        field.line->setPlaceholderText("comma-separated");
      }
      row_layout->addWidget(field.line, /*stretch=*/1);
    }

    auto* reset_button = new QPushButton("Reset", row_widget);
    reset_button->setMaximumWidth(56);
    reset_button->setToolTip(
        QString("Reset to default: %1").arg(QString::fromStdString(field.entry.default_display)));
    connect(reset_button, &QPushButton::clicked, this, [this, i] { ResetField(i); });
    row_layout->addWidget(reset_button);

    auto* label = new QLabel(QString::fromStdString(field.entry.key), this);
    label->setToolTip(QString::fromStdString(field.entry.doc));
    row_widget->setToolTip(QString::fromStdString(field.entry.doc));

    field.row_widget = row_widget;
    form_->addRow(label, row_widget);
  }

  SetAdvancedVisible(show_advanced_->isChecked());
}

void SettingsDialog::SetAdvancedVisible(bool show) {
  for (const Field& field : fields_) {
    if (field.entry.tier != "basic") form_->setRowVisible(field.row_widget, show);
  }
}

std::string SettingsDialog::CurrentText(const Field& field) const {
  if (field.check) return field.check->isChecked() ? "true" : "false";
  return field.line->text().toStdString();
}

void SettingsDialog::SetFieldText(Field& field, const std::string& text) {
  if (field.check) {
    field.check->setChecked(text == "true");
  } else {
    field.line->setText(QString::fromStdString(text));
  }
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
