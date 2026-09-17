#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QQueue>
#include <QSet>
#include <QSize>
#include <QString>

#include <string>

#include "../client/Types.h"

namespace mira_gui {

// Cover art for the library: the real thing when mirad has it, the
// generated placeholder when it doesn't, and one place that knows which is
// which.
//
// `GET /v1/games/{id}/artwork` is per game and answers 404 for anything
// never fetched, so a library of any size is a burst of requests where most
// come back empty. Three rules keep that from being the frontend's problem:
//
//  - **Ask once.** An id that has answered, either way, is never asked
//    again until something invalidates it — a `game.metadata_ready` event,
//    or an explicit refresh.
//  - **At most `kMaxInFlight` at a time.** Every request is a thread and a
//    socket (client/Async.h), and a 500-game library would otherwise open
//    500 of both the moment the window appears.
//  - **Keep the original, scale on demand.** The zoom slider changes the
//    tile size constantly; re-decoding a JPEG per step would be visible,
//    and re-fetching it would be absurd.
//
// Everything here is main-thread only, which is where the client delivers
// its results anyway.
class ArtworkStore : public QObject {
  Q_OBJECT

public:
  explicit ArtworkStore(QObject* parent = nullptr);

  // The cover to draw for this game at this tile size. Never empty: a game
  // with no artwork gets its placeholder, so a caller never has to decide
  // what to show instead. The first call for an id also queues the fetch.
  QPixmap Cover(const GameSummary& game, QSize tile, qreal device_pixel_ratio);

  // True only if real artwork is held for this id. False covers both "asked
  // and there was none" and "not asked yet", which is what a bulk re-fetch
  // wants: neither one has a cover to show.
  bool HasArtwork(const std::string& id) const;

  // Forget everything known about one game's artwork and fetch it again.
  // For `game.metadata_ready`, and for an explicit refresh.
  void Invalidate(const std::string& id);

  // Drop the scaled copies only — the originals are still good. For a
  // rename, which changes the placeholder's initials but not the artwork.
  void InvalidateRendering(const std::string& id);

signals:
  // Real artwork arrived (or was dropped) for this id; whatever is drawing
  // it should ask for the cover again.
  void CoverChanged(const QString& id);

private:
  void Request(const QString& id);
  void Pump();

  static constexpr int kMaxInFlight = 4;

  QHash<QString, QPixmap> original_;  // by id, at whatever size mirad sent
  QHash<QString, QPixmap> scaled_;    // by "id@tile_width"
  QSet<QString> answered_;            // asked and heard back, either way
  QSet<QString> queued_;              // in `pending_` or in flight
  QQueue<QString> pending_;
  int in_flight_ = 0;
};

}  // namespace mira_gui
