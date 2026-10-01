#pragma once

#include <QWidget>

#include <string>
#include <vector>

#include "../client/Types.h"
#include "../ui/SettingEditor.h"

class QLabel;
class QPushButton;

namespace mira_gui {

class SettingsNavWidget;

// The "just for this game" half of GameDetailDialog: every overridable
// setting, resolved through default -> settings.toml -> this game, editable
// the same way SettingsDialog edits the global value, plus a Clear that
// drops back to the layer underneath.
//
// Its own widget because it talks to a different endpoint than the dialog
// around it: the dialog saves fields via PATCH .../games/{id}, this saves
// keys via PATCH .../games/{id}/config. The dialog only asks it what changed.
class OverridesEditor : public QWidget {
  Q_OBJECT

public:
  explicit OverridesEditor(std::string game_id, QWidget* parent = nullptr);

  // Fetches the schema, builds a row per key, then fills in this game's
  // resolved values. Non-fatal if either request fails: the rest of the
  // dialog still works without this section.
  void Load();

  // The keys whose value the user actually changed, ready for
  // PATCH /v1/games/{id}/config. Empty when nothing was touched, so a dialog
  // opened and saved unedited sends no override patch at all.
  std::vector<GameConfigEdit> PendingEdits() const;

  // Call after PendingEdits() was successfully applied. It resets the
  // change-tracking baseline without a re-fetch, so PendingEdits() goes
  // back to empty.
  void MarkSaved();

private:
  // One row: an overridable schema key with the same editor the settings
  // screen gives it, the layer the shown value came from, and a Clear button
  // that only means something once that layer is "game".
  struct Field : SettingEditor {
    std::string layer;  // "default" | "config" | "game"
    QLabel* layer_label = nullptr;
    QPushButton* reset_button = nullptr;
  };

  void BuildRows(const ConfigSchemaResult& schema);
  void ApplyValues(const GameConfigResult& config);
  void Reload();
  void ResetField(size_t index);

  std::string game_id_;
  SettingsNavWidget* nav_ = nullptr;
  std::vector<Field> fields_;
};

}  // namespace mira_gui
