#include "OverridesEditor.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSizePolicy>

#include "../ui/Notify.h"
#include "../ui/SettingsNav.h"
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "../client/MiradClient.h"

namespace mira_gui {

OverridesEditor::OverridesEditor(std::string game_id, QWidget* parent)
    : QWidget(parent), game_id_(std::move(game_id)) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);

  nav_ = new SettingsNavWidget(this);
  outer->addWidget(nav_);
}

void OverridesEditor::Load() {
  MiradClient::GetConfigSchemaAsync(this, [this](ConfigSchemaResult schema) {
    if (!schema.ok) return;  // non-fatal: the main game fields still work without this section
    BuildRows(schema);
    Reload();
  });
}

void OverridesEditor::Reload() {
  MiradClient::GetGameConfigAsync(this, game_id_, [this](GameConfigResult config) {
    if (config.ok) ApplyValues(config);
  });
}

void OverridesEditor::BuildRows(const ConfigSchemaResult& schema) {
  // Only what the schema marks per-game, in the same order and categories as
  // the main Settings screen.
  std::vector<ConfigSchemaEntry> entries;
  for (const ConfigSchemaEntry& entry : schema.entries) {
    if (entry.per_game) entries.push_back(entry);
  }
  std::vector<std::string> categories;
  for (const ConfigSchemaEntry& entry : entries) categories.push_back(entry.category);

  for (const auto& [category, rows] : GroupByCategory(categories)) {
    QFormLayout* form = nav_->AddCategory(category);
    int group = entries[rows.front()].group;

    for (const size_t i : rows) {
      if (entries[i].group != group) {
        nav_->AddDivider(form);
        group = entries[i].group;
      }

      const ConfigSchemaEntry& entry = entries[i];
      Field field;
      field.entry = entry;

      auto* row_widget = new QWidget(this);
      auto* row_layout = new QHBoxLayout(row_widget);
      row_layout->setContentsMargins(0, 0, 0, 0);
      row_layout->setSpacing(8);

      if (entry.type == "a boolean") {
        field.check = new QCheckBox(row_widget);
        row_layout->addWidget(field.check);
      } else {
        field.line = new QLineEdit(row_widget);
        if (entry.type == "an array of strings") field.line->setPlaceholderText("comma-separated");
        if (entry.is_secret) field.line->setEchoMode(QLineEdit::PasswordEchoOnEdit);
        row_layout->addWidget(field.line, /*stretch=*/1);
      }

      field.layer_label = new QLabel(row_widget);
      field.layer_label->setProperty("role", "muted");
      field.layer_label->setMinimumWidth(56);
      row_layout->addWidget(field.layer_label);

      // Only meaningful once this game actually has an override to remove —
      // disabled until ApplyValues confirms layer == "game", since there's
      // nothing to clear otherwise.
      field.reset_button = new QPushButton("Clear", row_widget);
      field.reset_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
      field.reset_button->setEnabled(false);
      field.reset_button->setToolTip("No per-game override set for this key");
      row_layout->addWidget(field.reset_button);

      const QString label_text =
          QString::fromStdString(entry.label.empty() ? entry.key : entry.label);
      auto* label = new QLabel(label_text, this);
      label->setToolTip(QString::fromStdString(entry.doc));
      row_widget->setToolTip(QString::fromStdString(entry.doc));

      field.row_widget = row_widget;
      fields_.push_back(field);
      const size_t index = fields_.size() - 1;
      connect(field.reset_button, &QPushButton::clicked, this, [this, index] { ResetField(index); });
      form->addRow(label, row_widget);
      nav_->RegisterRow(form, row_widget,
                        QString("%1 %2 %3 %4").arg(QString::fromStdString(entry.key), label_text,
                                                    category, QString::fromStdString(entry.doc)));
    }
  }
}

void OverridesEditor::ApplyValues(const GameConfigResult& config) {
  // BuildRows makes one row per schema key, since only this per-game
  // response says which are overridable — daemon-only keys are hidden here
  // rather than never built.
  for (const GameConfigEntry& entry : config.entries) {
    const auto it = std::ranges::find(fields_, entry.key,
                                      [](const Field& f) { return f.entry.key; });
    if (it == fields_.end()) continue;
    Field& field = *it;

    nav_->SetRowGateVisible(field.row_widget, entry.overridable);
    if (!entry.overridable) continue;

    field.layer = entry.layer;
    field.layer_label->setText(QString("(%1)").arg(QString::fromStdString(entry.layer)));
    field.reset_button->setEnabled(entry.layer == "game");
    field.reset_button->setToolTip(
        entry.layer == "game" ? "Remove this game's override, falling back to the setting below it"
                              : "No per-game override set for this key");
    if (field.check) {
      field.check->setChecked(entry.value_display == "true");
    } else {
      field.line->setText(QString::fromStdString(entry.value_display));
      field.line->setCursorPosition(0);
    }
    field.original = CurrentText(field);
  }
}

std::string OverridesEditor::CurrentText(const Field& field) const {
  if (field.check) return field.check->isChecked() ? "true" : "false";
  return field.line->text().toStdString();
}

std::vector<GameConfigEdit> OverridesEditor::PendingEdits() const {
  std::vector<GameConfigEdit> edits;
  for (const Field& field : fields_) {
    // An empty layer means ApplyValues never reached this row (not
    // overridable, or the fetch failed), so there is nothing to compare
    // against and nothing to send.
    const std::string current = CurrentText(field);
    if (field.layer.empty() || current == field.original) continue;
    edits.push_back(GameConfigEdit{field.entry.key, field.entry.type, current, false});
  }
  return edits;
}

void OverridesEditor::MarkSaved() {
  for (Field& field : fields_) field.original = CurrentText(field);
}

void OverridesEditor::ResetField(size_t index) {
  const Field& field = fields_[index];
  MiradClient::PatchGameConfigAsync(
      this, game_id_, {GameConfigEdit{field.entry.key, field.entry.type, std::string(), true}},
      [this](PatchGameConfigResult result) {
        if (!result.ok) {
          notify::Failed(this, "Could not reset this override.",
                         QString::fromStdString(result.error));
          return;
        }
        Reload();
      });
}

}  // namespace mira_gui
