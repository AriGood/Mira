#pragma once

#include <QWidget>
#include <QString>

#include <string>
#include <vector>

#include "../client/MiradClient.h"

namespace mira_gui {
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
// directly in a window (GameDetailsPanel, taking over the sidebar).
class GameEditForm : public QWidget {
  Q_OBJECT

public:
  explicit GameEditForm(std::string id, QWidget* parent = nullptr);

  const std::string& id() const { return id_; }

  void Save();

  // True once any field differs from what Populate() last loaded or Save()
  // last confirmed — the signal a caller uses to warn before discarding.
  bool IsDirty() const;

signals:
  void Loaded(QString name);
  void LoadFailed(QString error);
  void SaveFinished(bool ok, QString error);

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
  QLabel* status_label_;
  QLabel* install_path_label_;
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
