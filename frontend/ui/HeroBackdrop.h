#pragma once

#include <QPixmap>
#include <QWidget>

#include <string>

#include "../client/Types.h"

namespace mira_gui {

class ArtworkStore;

// A card whose top is a game's hero art, darkening into the card's surface
// so whatever is laid over it stays readable. A game with no hero gets its
// cover, blurred; one with neither, its tile's generated art, blurred.
class HeroBackdrop : public QWidget {
  Q_OBJECT

public:
  explicit HeroBackdrop(ArtworkStore* artwork, QWidget* parent = nullptr);

  void ShowGame(const GameSummary& game);
  // A new cover or hero for `id` (safe to call for any game).
  void RefreshCover(const std::string& id);
  void RefreshHero(const std::string& id);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void LoadHero();
  QPixmap Source() const;

  ArtworkStore* artwork_ = nullptr;
  GameSummary game_;
  QPixmap hero_;
  // Source() scaled and blurred for the current size; rebuilt on a change.
  QPixmap rendered_;
  qint64 rendered_key_ = 0;
};

}  // namespace mira_gui
