#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include "../client/MiradClient.h"

namespace mira_gui {
class OverridesEditor;
}

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QWidget;

// Shows one game's full record (GET /v1/games/{id}) and lets the user
// correct the fields auto-detection got wrong, saving via
// PATCH /v1/games/{id}. Opened by double-clicking a row in MainWindow's
// table.
//
// Doesn't touch MainWindow's table directly on save: mirad publishes
// game.updated on a successful PATCH (docs/api.md), and MainWindow's
// existing event stream already patches the row from that — reusing the
// same path a background auto-detection update would take, rather than
// a second, parallel update mechanism.
class GameDetailDialog : public QDialog {
  Q_OBJECT

public:
  GameDetailDialog(std::string id, QWidget* parent = nullptr);

private:
  void Load();
  void Populate(const mira_gui::GameDetail& game);
  void PopulateExeCombo(const std::vector<mira_gui::GameDetail::Candidate>& candidates,
                        const std::string& current);
  void PopulateRunnerCombo(const mira_gui::RunnersResult& result);
  void Save();
  void OnExeComboActivated(int index);
  void OnRunnerComboActivated(int index);
  void BrowseExecutable();
  void SetAdvancedVisible(bool show);

  std::string id_;
  std::string install_path_;

  QLabel* status_label_;
  QLabel* install_path_label_;
  QLabel* confidence_label_;
  QLabel* last_error_label_;
  QLineEdit* name_edit_;
  QComboBox* exe_combo_;
  QLineEdit* args_edit_;
  QLineEdit* working_dir_edit_;
  QComboBox* runner_combo_;

  QCheckBox* show_advanced_;
  QWidget* advanced_container_;
  QLineEdit* data_dir_edit_;
  QPlainTextEdit* runner_config_edit_;
  QPlainTextEdit* env_edit_;
  mira_gui::OverridesEditor* overrides_;

  QPushButton* save_button_;
};
