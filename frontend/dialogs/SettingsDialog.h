#pragma once

#include <QDialog>
#include <QString>

#include <string>
#include <vector>

#include "../client/MiradClient.h"

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

// A settings screen generated entirely from GET /v1/config/schema
// (docs/api.md) — almost no setting is hardcoded here, so a new entry in
// src/config/Schema.cpp appears with zero frontend changes. Basic-tier
// settings show by default; advanced/expert fold behind a checkbox
// (docs/api.md: "a frontend generating a settings UI from this should show
// basic by default and fold the rest behind a disclosure, never omit them").
//
// Two deliberate exceptions to "no hardcoded setting names":
// - "default_runner.windows" gets a runner-picker combo instead of a plain
//   text box (see kRunnerKeys and BuildRows) — GET /v1/runners exists
//   precisely to make a runner reference pickable instead of hand-typed.
// - Rows are grouped into named sections (Library, Runners, Detection, ...)
//   for readability; the grouping itself is a small key->category table
//   (CategoryFor, SettingsDialog.cpp) rather than derived from the schema,
//   since the schema has no category concept. A schema key missing from
//   that table still appears — grouped by guessing from its dotted prefix,
//   or under "General" — so a brand-new setting is never silently dropped.
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
    QComboBox* combo = nullptr;     // set instead of `line` for kRunnerKeys
    QWidget* row_widget = nullptr;  // the field column's widget, for setRowVisible
    QFormLayout* owner_form = nullptr;  // the category group's form this row lives in
  };

  // One named section of the dialog (e.g. "Library"). `has_basic` is
  // whether any field in it is basic-tier — such a group is always shown,
  // since hiding it would also hide a setting the "show advanced" toggle
  // promised to keep visible.
  struct CategoryGroup {
    QGroupBox* box = nullptr;
    QFormLayout* form = nullptr;
    bool has_basic = false;
  };

  void Load();
  void LoadFrontendPrefs();
  void BuildInterfaceGroup();
  void BuildRows();
  void PopulateRunnerCombos(const mira_gui::RunnersResult& result);
  void SetAdvancedVisible(bool show);
  void ResetField(size_t index);
  void Save();
  std::string CurrentText(const Field& field) const;
  void SetFieldText(Field& field, const std::string& text);

  QVBoxLayout* groups_layout_ = nullptr;
  QCheckBox* show_advanced_ = nullptr;
  // The frontend's own settings, kept visually and mechanically apart from
  // the schema-driven ones below: these are written to frontend.toml via
  // the opaque `frontend` key, never to settings.toml, because no schema
  // entry describes them and the daemon has no opinion about them.
  QCheckBox* scan_on_startup_ = nullptr;
  bool scan_on_startup_original_ = true;
  QComboBox* notifications_ = nullptr;
  QString notifications_original_;
  std::vector<Field> fields_;
  std::vector<CategoryGroup> groups_;
  QPushButton* save_button_ = nullptr;
};
