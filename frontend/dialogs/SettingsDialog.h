#pragma once

#include <QDialog>

namespace mira_gui {
class SettingsPanel;
}

// A thin QDialog shell around ui/SettingsPanel — the modal route to
// settings. The embedded route is LibraryWindow swapping SettingsPanel into
// its central stack directly; both share the one panel implementation.
class SettingsDialog : public QDialog {
  Q_OBJECT

public:
  explicit SettingsDialog(QWidget* parent = nullptr);

  // Warns before discarding unsaved changes — covers Cancel, Esc, and the
  // titlebar close button, which all route through reject().
  void reject() override;

private:
  mira_gui::SettingsPanel* panel_ = nullptr;
};
