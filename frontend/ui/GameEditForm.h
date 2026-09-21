#pragma once

#include <QWidget>
#include <QString>

#include <string>
#include <vector>

#include "../client/MiradClient.h"

namespace mira_gui {
class ArtworkStore;
class HeroArtWidget;
class OverridesEditor;
}

class QComboBox;
class QDialog;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QWidget;

namespace mira_gui {

// One game's editable record (GET/PATCH /v1/games/{id}), with no QDialog
// machinery — embeddable in a dialog shell (dialogs/GameDetailDialog) or
// directly in a window (LibraryWindow, full-width).
class GameEditForm : public QWidget {
  Q_OBJECT

public:
  explicit GameEditForm(std::string id, QWidget* parent = nullptr);

  const std::string& id() const { return id_; }

  // Same store the grid's own tiles use. Without one this form's own copy
  // still works and shows placeholders.
  void SetArtworkStore(ArtworkStore* store);

  void Save();

  // True once any field differs from what Populate() last loaded or Save()
  // last confirmed — the signal a caller uses to warn before discarding.
  bool IsDirty() const;

  // For game.artwork_selected/a fresh cover fetch to reach this form's own
  // hero/cover box while it's open — both are safe no-ops if this isn't the
  // game currently on screen (HeroArtWidget's own concern, see its header).
  void RefreshCover();
  void RefreshBanner(const std::string& id);

signals:
  void Loaded(QString name);
  void LoadFailed(QString error);
  void SaveFinished(bool ok, QString error);
  // "cover" or "hero" — a caller opens ArtworkPickerDialog for it.
  void ArtworkPickRequested(QString slot);

private:
  void Load();
  void Populate(const mira_gui::GameDetail& game);
  void PopulateExeCombo(const std::vector<mira_gui::GameDetail::Candidate>& candidates,
                        const std::string& current);
  void PopulateRunnerCombo(const mira_gui::RunnersResult& result);
  void OnExeComboActivated(int index);
  void OnRunnerComboActivated(int index);
  void BrowseExecutable();
  void OpenAdvanced();
  mira_gui::GamePatch CurrentPatch() const;

  std::string id_;
  std::string install_path_;
  mira_gui::GamePatch original_patch_;

  QFormLayout* form_ = nullptr;
  HeroArtWidget* hero_art_ = nullptr;
  QLabel* status_label_;
  QLabel* install_path_label_;
  QLabel* source_note_label_;
  QLabel* last_error_label_;
  QLineEdit* name_edit_;
  QComboBox* exe_combo_;
  QLineEdit* args_edit_;
  QLineEdit* working_dir_edit_;
  QLineEdit* tags_edit_;
  QComboBox* runner_combo_;

  // Non-modal window holding just the per-game overrides table (its own
  // category tabs, one row per overridable key) — long enough on its own to
  // cramp the sidebar if it were inline.
  QDialog* advanced_dialog_ = nullptr;
  QLineEdit* data_dir_edit_;
  QPlainTextEdit* runner_config_edit_;
  QPlainTextEdit* env_edit_;
  mira_gui::OverridesEditor* overrides_;
};

}  // namespace mira_gui
