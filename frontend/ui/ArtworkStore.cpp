#include "ArtworkStore.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include "../client/MiradClient.h"
#include "CoverArt.h"

namespace mira_gui {
namespace {

QString ScaleKey(const QString& id, QSize tile) {
  return QString("%1@%2").arg(id).arg(tile.width());
}

// Real artwork is whatever aspect ratio the source happened to use — Steam's
// CDN library capsules are 2:3, SteamGridDB's are not always. The tile is
// 2:3, so the image is scaled to cover and centre-cropped rather than
// letterboxed: a band of background around a cover reads as a broken image,
// while a cropped edge reads as a cover.
QPixmap FitToTile(const QPixmap& source, QSize tile, qreal device_pixel_ratio) {
  const QSize target = tile * device_pixel_ratio;
  const QPixmap filled =
      source.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

  QPixmap out(target);
  out.fill(Qt::transparent);
  {
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(target)), 6 * device_pixel_ratio,
                        6 * device_pixel_ratio);
    painter.setClipPath(clip);
    painter.drawPixmap((target.width() - filled.width()) / 2,
                       (target.height() - filled.height()) / 2, filled);
  }
  out.setDevicePixelRatio(device_pixel_ratio);
  return out;
}

}  // namespace

ArtworkStore::ArtworkStore(QObject* parent) : QObject(parent) {}

QPixmap ArtworkStore::Cover(const GameSummary& game, QSize tile, qreal device_pixel_ratio) {
  const QString id = QString::fromStdString(game.id);
  const QString key = ScaleKey(id, tile);

  if (const auto cached = scaled_.constFind(key); cached != scaled_.constEnd()) return *cached;

  if (!answered_.contains(id)) Request(id);

  QPixmap cover;
  if (const auto art = original_.constFind(id); art != original_.constEnd()) {
    cover = FitToTile(*art, tile, device_pixel_ratio);
  } else {
    // The placeholder is keyed on the id, not the name, so it survives a
    // rename and can be learned by sight — see CoverArt.h.
    cover = PlaceholderCover(QString::fromStdString(game.name), id,
                             QSize(tile.width() - 10, tile.height() - 10), device_pixel_ratio);
  }
  scaled_.insert(key, cover);
  return cover;
}

void ArtworkStore::Invalidate(const std::string& id) {
  const QString key = QString::fromStdString(id);
  original_.remove(key);
  answered_.remove(key);
  InvalidateRendering(id);
  Request(key);
}

void ArtworkStore::InvalidateRendering(const std::string& id) {
  // Every scaled copy of this game, not just the one at the current tile
  // size: the zoom slider leaves entries behind at every size it passed
  // through, and a stale one would come back the moment it returned there.
  const QString prefix = QString::fromStdString(id) + "@";
  for (auto it = scaled_.begin(); it != scaled_.end();) {
    it = it.key().startsWith(prefix) ? scaled_.erase(it) : std::next(it);
  }
}

void ArtworkStore::Request(const QString& id) {
  if (queued_.contains(id)) return;
  queued_.insert(id);
  pending_.enqueue(id);
  Pump();
}

void ArtworkStore::Pump() {
  while (in_flight_ < kMaxInFlight && !pending_.isEmpty()) {
    const QString id = pending_.dequeue();
    ++in_flight_;
    MiradClient::GetArtworkAsync(this, id.toStdString(), [this, id](ArtworkResult result) {
      --in_flight_;
      queued_.remove(id);
      // Answered covers all three outcomes on purpose. A 404 means "nothing
      // cached", and re-asking on every repaint would turn an empty library
      // into a request loop; an unreachable daemon is no different, because
      // the reconnect will re-fetch the library and that is when to try
      // again. Only Invalidate reopens the question.
      answered_.insert(id);

      if (result.ok) {
        QPixmap art;
        // Loaded from bytes rather than a path: the image lives in mirad's
        // own cache directory, which the frontend has no business knowing
        // the layout of (docs/api.md serves it for exactly this reason).
        if (art.loadFromData(reinterpret_cast<const uchar*>(result.bytes.data()),
                             static_cast<uint>(result.bytes.size()))) {
          original_.insert(id, art);
          InvalidateRendering(id.toStdString());
          emit CoverChanged(id);
        }
      }
      Pump();
    });
  }
}

}  // namespace mira_gui
