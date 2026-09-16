#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include "MiradClient.h"

class QCheckBox;
class QFormLayout;
class QLineEdit;
class QPushButton;

// A settings screen generated entirely from GET /v1/config/schema
// (docs/api.md) — no setting is hardcoded here, so a new entry in
// src/config/Schema.cpp appears with zero frontend changes. Basic-tier
// settings show by default; advanced/expert fold behind a checkbox
// (docs/api.md: "a frontend generating a settings UI from this should show
// basic by default and fold the rest behind a disclosure, never omit them").
class SettingsDialog : public QDialog {
  Q_OBJECT

public:
  explicit SettingsDialog(QWidget* parent = nullptr);

private:
  struct Field {
    mira_gui::ConfigSchemaEntry entry;
    std::string original;  // last value loaded from the daemon, for change detection
    QCheckBox* check = nullptr;
    QLineEdit* line = nullptr;
    QWidget* row_widget = nullptr;  // the field column's widget, for setRowVisible
  };

  void Load();
  void BuildRows();
  void SetAdvancedVisible(bool show);
  void ResetField(size_t index);
  void Save();
  std::string CurrentText(const Field& field) const;
  void SetFieldText(Field& field, const std::string& text);

  QFormLayout* form_;
  QCheckBox* show_advanced_;
  std::vector<Field> fields_;
  QPushButton* save_button_;
};
