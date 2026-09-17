#pragma once

#include <QDialog>

namespace mira_gui {
class SettingsPanel;
}

// A thin QDialog shell around ui/SettingsPanel — the modal route to
// settings (MainWindow's classic view, or LibraryWindow's "Settings…"
// window action for someone who doesn't want the grid replaced). The
// embedded route is LibraryWindow swapping SettingsPanel into its central
// stack directly; both share the one panel implementation.
class SettingsDialog : public QDialog {
  Q_OBJECT

public:
  explicit SettingsDialog(QWidget* parent = nullptr);

private:
  mira_gui::SettingsPanel* panel_ = nullptr;
};
