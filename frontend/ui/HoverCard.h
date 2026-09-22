#pragma once

#include <QWidget>

#include <string>

#include "../client/Types.h"

class QLabel;

namespace mira_gui {

// A floating, non-modal preview shown after a short dwell over a grid tile
// -- the old right sidebar's metadata (name, status, ProtonDB tier,
// platform/runner, last played, playtime, developer/genres), without a
// permanent panel taking up space all the time. Install path, description
// and reviews are deliberately left out: this is a glance, not the sidebar
// reborn -- the per-game edit page and GameDetailPageDialog still have them.
class HoverCard : public QWidget {
  Q_OBJECT

public:
  explicit HoverCard(QWidget* parent = nullptr);

  // Fills in what's already known (no round trip), then fetches the rest
  // (developer/genres/ProtonDB tier) with one GET /v1/games/{id}/metadata.
  // Safe to call again for a different game while still visible -- the
  // fetch result is dropped if a newer ShowGame has since moved on.
  void ShowGame(const GameSummary& game, bool running);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void ShowMetadata(const GameMetadata& metadata);

  std::string game_id_;
  QLabel* name_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* protondb_ = nullptr;
  QLabel* platform_line_ = nullptr;
  QLabel* played_line_ = nullptr;
  QLabel* developer_line_ = nullptr;
};

}  // namespace mira_gui
