#pragma once

#include <QDialog>
#include <QString>

#include <string>

namespace mira_gui {

// Everything mirad cached about a game that has no room anywhere else in the
// library view: description, release, reviews, genres, requirements,
// screenshots, trailers, DLC. Reached from the right-click "More details…".
// Which parts exist depends on where the metadata came from.
class GameDetailPageDialog : public QDialog {
  Q_OBJECT

public:
  GameDetailPageDialog(std::string game_id, QString game_name, QWidget* parent = nullptr);

private:
  std::string game_id_;
};

}  // namespace mira_gui
