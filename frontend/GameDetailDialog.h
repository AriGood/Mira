#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include "MiradClient.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

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
  void RenderCandidates();
  void Save();
  void UseCandidate(const std::string& rel_path);
  void BrowseExecutable();

  std::string id_;
  std::string install_path_;
  std::vector<mira_gui::GameDetail::Candidate> candidates_;

  QLabel* status_label_;
  QLabel* install_path_label_;
  QLabel* confidence_label_;
  QLabel* last_error_label_;
  QLineEdit* name_edit_;
  QLineEdit* exe_path_edit_;
  QLineEdit* args_edit_;
  QLineEdit* working_dir_edit_;
  QLineEdit* runner_ref_edit_;
  QVBoxLayout* candidates_layout_;
  QPushButton* save_button_;
};
