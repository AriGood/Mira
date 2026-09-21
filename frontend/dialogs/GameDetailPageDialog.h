#pragma once

#include <QDialog>
#include <QString>

#include <string>

namespace mira_gui {

// The wider store-info page that has no room anywhere else in the library
// view: screenshots, trailers, requirements, DLC, content descriptors,
// achievements. Reached from the grid's right-click "More details…".
class GameDetailPageDialog : public QDialog {
  Q_OBJECT

public:
  // `runner_ref` decides eligibility: only a "steam:"-owned game ever has
  // any of these fields cached, so a non-Steam game skips the fetch entirely.
  GameDetailPageDialog(std::string game_id, QString game_name, const std::string& runner_ref,
                       QWidget* parent = nullptr);

private:
  std::string game_id_;
};

}  // namespace mira_gui
