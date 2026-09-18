#pragma once

#include <QWidget>
#include <QString>

#include <string>
#include <vector>

#include "../client/MiradClient.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace mira_gui {

// The settings screen's content, with no QDialog machinery — embeddable in
// a dialog shell (dialogs/SettingsDialog) or directly in a window (the
// library grid, taking over its body). Owns loading the schema, building
// one form per category, and saving both the backend config and the
// frontend-only prefs above it.
class SettingsPanel : public QWidget {
  Q_OBJECT

public:
  explicit SettingsPanel(QWidget* parent = nullptr);

  // Patches every changed field. Emits SaveFinished either way; the caller
  // decides what "done" means (close a dialog, switch back to the grid).
  void Save();

  // True if anything differs from what Load() last fetched or Save() last
  // confirmed — the signal a caller uses to warn before discarding.
  bool IsDirty() const;

signals:
  void LoadFailed(QString error);
  void SaveFinished(bool ok, QString error);

private:
  struct Field {
    mira_gui::ConfigSchemaEntry entry;
    std::string original;  // last value loaded from the daemon, for change detection
    QCheckBox* check = nullptr;
    QLineEdit* line = nullptr;
    QComboBox* combo = nullptr;     // set instead of `line` for a runner key or a schema enum
    QDoubleSpinBox* spin = nullptr;  // set instead of `line` for a bounded number
    QWidget* row_widget = nullptr;  // the field column's widget, for setRowVisible
    QFormLayout* owner_form = nullptr;  // the category group's form this row lives in
  };

  struct CategoryGroup {
    int tab_index = -1;
    QFormLayout* form = nullptr;
    bool has_basic = false;
  };

  void Load();
  void LoadFrontendPrefs();
  void BuildInterfaceGroup();
  void BuildRows();
  // Adds a tab (scroll-wrapped) and returns its form layout, ready for rows.
  QFormLayout* AddCategoryTab(const QString& title);
  void PopulateRunnerCombos(const mira_gui::RunnersResult& result);
  void SetAdvancedVisible(bool show);
  void ResetField(size_t index);
  std::string CurrentText(const Field& field) const;
  void SetFieldText(Field& field, const std::string& text);

  QTabWidget* tabs_ = nullptr;
  QCheckBox* show_advanced_ = nullptr;
  QCheckBox* scan_on_startup_ = nullptr;
  bool scan_on_startup_original_ = true;
  QComboBox* notifications_ = nullptr;
  QString notifications_original_;
  QSpinBox* notification_timeout_ = nullptr;
  int notification_timeout_original_ = 0;
  QCheckBox* menu_bar_pinned_ = nullptr;
  bool menu_bar_pinned_original_ = false;
  QCheckBox* game_settings_in_sidebar_ = nullptr;
  bool game_settings_in_sidebar_original_ = true;
  std::vector<Field> fields_;
  std::vector<CategoryGroup> groups_;
};

}  // namespace mira_gui
