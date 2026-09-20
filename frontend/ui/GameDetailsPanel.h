#pragma once

#include <QString>
#include <QWidget>

#include <QHash>
#include <QPixmap>

#include <optional>
#include <string>

#include "../client/Types.h"

class QFormLayout;
class QLabel;
class QPushButton;
class QResizeEvent;
class QStackedWidget;

namespace mira_gui {

class ArtworkStore;

// The side panel a selected game fills in: cover, name, status, the
// play/stop button, and its metadata.
//
// Split out of LibraryWindow because it holds state of its own — which
// game it last fetched a path for, so rebuilding the grid on every
// keystroke doesn't re-issue the request or flicker the field. Reports
// what the user asked for and lets the window decide what that means.
class GameDetailsPanel : public QWidget {
  Q_OBJECT

public:
  explicit GameDetailsPanel(QWidget* parent = nullptr);

  // The store the panel's cover comes from. Shared with the grid so the
  // same image is fetched, decoded and cached once for both. Without one
  // the panel still works and shows placeholders.
  void SetArtworkStore(ArtworkStore* store);

  void ShowGame(const GameSummary& game, bool running);
  void Clear();

  // Re-draw the cover for the game currently on screen. For when artwork
  // arrives after the panel was filled in.
  void RefreshCover(const GameSummary& game);

  // Drops the cached hero banner for `id` and reloads it if that's the game
  // currently shown. For game.artwork_selected on the hero slot.
  void RefreshBanner(const std::string& id);

protected:
  void resizeEvent(QResizeEvent* event) override;

signals:
  void PlayRequested(const QString& id);
  void StopRequested(const QString& id);
  void EditRequested(const QString& id);
  void MetadataRefreshRequested(const QString& id);
  // `slot` is "cover" or "hero".
  void ArtworkPickRequested(const QString& id, const QString& slot);

private:
  // GET /v1/games/{id}/metadata, once per selection. What comes back is
  // cached store info, so a repeat selection costs one socket round trip and
  // no network.
  void LoadMetadata(const std::string& id);
  void ShowMetadata(const GameMetadata& metadata);
  void ClearMetadata();
  void LoadBanner(const std::string& id);
  void RenderBanner();
  // The image area a hero and, when there's no hero, a game's cover alike
  // get fit-and-letterboxed into (see FitLetterboxed in the .cpp).
  QSize ImageBoxSize() const;
  // Shows exactly one of banner_/cover_/placeholder_, based on hero_known_
  // and whether that hero's pixmap has actually arrived yet.
  void UpdateImageVisibility();

  QStackedWidget* stack_ = nullptr;
  QFormLayout* form_ = nullptr;
  QLabel* banner_ = nullptr;
  // Shown only while it's genuinely unknown whether this game has a hero —
  // never cover, so a game that turns out to have one never flashes its
  // cover first. See UpdateImageVisibility.
  QLabel* placeholder_ = nullptr;
  QLabel* description_ = nullptr;
  QLabel* released_ = nullptr;
  QLabel* developer_ = nullptr;
  QLabel* genres_ = nullptr;
  QLabel* reviews_ = nullptr;
  QLabel* protondb_ = nullptr;
  QLabel* cover_ = nullptr;
  QLabel* name_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* platform_ = nullptr;
  QLabel* runner_ = nullptr;
  QLabel* last_played_ = nullptr;
  QLabel* playtime_ = nullptr;
  QLabel* path_ = nullptr;
  QLabel* error_ = nullptr;
  QPushButton* play_ = nullptr;

  ArtworkStore* artwork_ = nullptr;
  std::string game_id_;
  bool running_ = false;
  // Kept only so a live hero_height change (theme::Notifier::Changed) can
  // regenerate the cover at the new size -- ArtworkStore bakes the target
  // size into the pixmap itself, so there's no cheaper way to resize it than
  // asking again with the same game.
  std::optional<GameSummary> current_game_;
  QHash<QString, QPixmap> banners_;  // hero art by id, at the size mirad sent
  QPixmap banner_source_;            // the one on screen, before scaling
  // Whether `id` has hero art, once LoadMetadata has resolved it this
  // session -- absent means "don't know yet". Lets a repeat selection skip
  // straight to banner_/cover_ with no placeholder, no cover-then-hero flash.
  QHash<QString, bool> hero_known_;
};

}  // namespace mira_gui
