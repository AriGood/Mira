#pragma once

#include <functional>
#include <string>

#include <QString>

#include "../client/Types.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QWidget;

namespace mira_gui {

// One schema setting's editor: a checkbox, enum or runner combo, bounded
// spin box, or line edit, picked from the entry's shape. Shared by the
// settings screen and a source page's settings card.
struct SettingEditor {
  ConfigSchemaEntry entry;
  std::string original;  // last value loaded from the daemon, for change detection
  QCheckBox* check = nullptr;
  QLineEdit* line = nullptr;
  QComboBox* combo = nullptr;      // set instead of `line` for a runner key or a schema enum
  QDoubleSpinBox* spin = nullptr;  // set instead of `line` for a bounded number
  QWidget* row_widget = nullptr;   // holds the editor, plus Reset for text and runner rows

  // Builds the row into `parent`. `on_reset` backs the Reset button; empty leaves it out.
  QWidget* Build(QWidget* parent, std::function<void()> on_reset);
  QWidget* BuildLabel(QWidget* parent) const;  // the label, with a [?] for the doc
  QString SearchText() const;                  // what the settings search matches on
  QWidget* Input() const;                      // the widget to focus
  std::string Text() const;
  void SetText(const std::string& text);
};

// Refills a runner combo: Auto, then every installed build, keeping the typed text.
void FillRunnerCombo(QComboBox* combo, const RunnersResult& runners);

}  // namespace mira_gui
