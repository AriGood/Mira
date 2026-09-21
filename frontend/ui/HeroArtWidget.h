#pragma once

#include <QHash>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include <optional>
#include <string>

#include "../client/Types.h"

class QLabel;
class QResizeEvent;

namespace mira_gui {

class ArtworkStore;

// A game's hero banner when mirad has one, its cover otherwise, or a
// placeholder while that's still unknown -- shared by GameDetailsPanel's
// sidebar and GameEditForm's own page so both show the same picture, sized
// the same way, kept in sync with each other's fetches for free.
class HeroArtWidget : public QWidget {
  Q_OBJECT

public:
  explicit HeroArtWidget(QWidget* parent = nullptr);

  // Shared with the grid so the same cover is fetched, decoded and cached
  // once. Without one this still works and shows placeholders.
  void SetArtworkStore(ArtworkStore* store);

  void ShowGame(const GameSummary& game);
  void Clear();

  // Re-renders the cover for the game currently on screen, without
  // re-fetching metadata -- for when artwork_ has new art for it.
  void RefreshCover();

  // Drops the cached hero banner for `id` and reloads it if that's the game
  // currently shown. For game.artwork_selected on the hero slot.
  void RefreshBanner(const std::string& id);

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  void LoadMetadata(const std::string& id);
  void LoadBanner(const std::string& id);
  void RenderBanner();
  QSize ImageBoxSize() const;
  // Shows exactly one of banner_/cover_/placeholder_, based on hero_known_
  // and whether that hero's pixmap has actually arrived yet.
  void UpdateImageVisibility();

  QLabel* banner_ = nullptr;
  // Shown only while it's genuinely unknown whether this game has a hero —
  // never cover, so a game that turns out to have one never flashes its
  // cover first. See UpdateImageVisibility.
  QLabel* placeholder_ = nullptr;
  QLabel* cover_ = nullptr;

  ArtworkStore* artwork_ = nullptr;
  std::string game_id_;
  std::optional<GameSummary> current_game_;
  QHash<QString, QPixmap> banners_;  // hero art by id, at the size mirad sent
  QPixmap banner_source_;            // the one on screen, before scaling
  // Whether `id` has hero art, once LoadMetadata has resolved it this
  // session -- absent means "don't know yet". Lets a repeat selection skip
  // straight to banner_/cover_ with no placeholder, no cover-then-hero flash.
  QHash<QString, bool> hero_known_;
};

}  // namespace mira_gui
