#pragma once

#include <QDialog>
#include <QString>

#include <string>

class QPlainTextEdit;

namespace mira_gui {

// Shows the tail of a game's own log file — GET /v1/games/{id}/log?lines=.
// Manual refresh only: mirad has no tail/stream endpoint for this, so there
// is nothing to subscribe to.
class LogViewerDialog : public QDialog {
  Q_OBJECT

public:
  LogViewerDialog(std::string game_id, QString game_name, QWidget* parent = nullptr);

private:
  void Refresh();

  std::string game_id_;
  QPlainTextEdit* text_ = nullptr;
};

}  // namespace mira_gui
