#include "ArtworkStore.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <optional>

#include "../client/Async.h"
#include "../client/MiradClient.h"
#include "CoverArt.h"
#include "Theme.h"

namespace mira_gui {
namespace {

const QSize kMaxArt(800, 1200);

QString ScaleKey(const QString& id, QSize tile) {
  return QString("%1@%2").arg(id).arg(tile.width());
}

// Real artwork isn't always 2:3 like the tile, so it's scaled to cover and
// centre-cropped rather than letterboxed: a cropped edge reads as a cover,
// a background band reads as a broken image.
//
// Rounded here to the live radius_tile, not a fixed constant: GameTileDelegate
// re-clips the grid's own copy to the same token on every paint, but a plain
// QLabel (the sidebar's cover) has no such second clip, so an unrounded or
// wrongly-rounded pixmap here would show through as-is.
QPixmap FitToTile(const QPixmap& source, QSize tile, qreal device_pixel_ratio) {
  const QSize target = tile * device_pixel_ratio;
  const QPixmap filled =
      source.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

  QPixmap out(target);
  out.fill(Qt::transparent);
  {
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    const int radius = theme::Current().radius_tile;
    QPainterPath clip;
    if (radius > 0) {
      clip.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(target)), radius * device_pixel_ratio,
                          radius * device_pixel_ratio);
    } else {
      clip.addRect(QRectF(QPointF(0, 0), QSizeF(target)));
    }
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
  return CoverById(QString::fromStdString(game.id), QString::fromStdString(game.name), tile,
                  device_pixel_ratio);
}

QPixmap ArtworkStore::TitleCover(const QString& source, const QString& ref, const QString& title, QSize tile,
                                 qreal device_pixel_ratio) {
  const QString id = source + "-" + ref;
  titles_.insert(id, {source.toStdString(), ref.toStdString()});
  return CoverById(id, title, tile, device_pixel_ratio);
}

QPixmap ArtworkStore::CoverById(const QString& id, const QString& name, QSize tile, qreal device_pixel_ratio) {
  const QString key = ScaleKey(id, tile);

  if (const auto cached = scaled_.constFind(key); cached != scaled_.constEnd()) return *cached;

  if (!answered_.contains(id)) Request(id);

  QPixmap cover;
  if (const auto art = original_.constFind(id); art != original_.constEnd()) {
    cover = FitToTile(*art, tile, device_pixel_ratio);
  } else {
    // Keyed on the id, not the name, so it survives a rename.
    cover = PlaceholderCover(name, id,
                             QSize(tile.width() - 10, tile.height() - 10), device_pixel_ratio);
  }
  scaled_.insert(key, cover);
  return cover;
}

bool ArtworkStore::HasArtwork(const std::string& id) const {
  return original_.contains(QString::fromStdString(id));
}

QPixmap ArtworkStore::RawArtwork(const std::string& id) const {
  return original_.value(QString::fromStdString(id));
}

void ArtworkStore::EnsureRequested(const std::string& id) {
  const QString key = QString::fromStdString(id);
  if (!answered_.contains(key)) Request(key);
}

void ArtworkStore::NoteArt(const std::string& id, const std::optional<ArtVersions>& art) {
  if (!art) return;
  const QString key = QString::fromStdString(id);
  const auto cover = art->find("cover");
  const QString version = cover != art->end() ? QString::fromStdString(cover->second) : QString();
  const auto known = versions_.constFind(key);
  const bool first = known == versions_.constEnd();
  if (!first && *known == version) return;
  versions_.insert(key, version);

  if (version.isEmpty()) {
    answered_.insert(key);
    if (original_.remove(key) > 0) {
      InvalidateRendering(id);
      emit CoverChanged(key);
    }
    return;
  }
  if (queued_.contains(key)) {
    ask_again_.insert(key);  // its answer may be the old image
  } else if (answered_.contains(key) && (!first || !original_.contains(key))) {
    // A first version with an image already in hand is that image.
    answered_.remove(key);
    Request(key);
  }
}

void ArtworkStore::Invalidate(const std::string& id) {
  const QString key = QString::fromStdString(id);
  original_.remove(key);
  answered_.remove(key);
  InvalidateRendering(id);
  Request(key);
}

void ArtworkStore::TitleArtworkReady(const std::string& id) {
  const QString key = QString::fromStdString(id);
  if (!titles_.contains(key) || original_.contains(key)) return;
  if (queued_.contains(key)) {
    ask_again_.insert(key);  // its answer may predate the fetch
  } else if (answered_.contains(key)) {
    Invalidate(id);
  }
}

void ArtworkStore::InvalidateRendering(const std::string& id) {
  // Every scaled copy, not just the current tile size: the zoom slider
  // leaves entries behind at every size it passed through.
  const QString prefix = QString::fromStdString(id) + "@";
  for (auto it = scaled_.begin(); it != scaled_.end();) {
    it = it.key().startsWith(prefix) ? scaled_.erase(it) : std::next(it);
  }
}

void ArtworkStore::InvalidateAllRenderings() { scaled_.clear(); }

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
    std::optional<std::pair<std::string, std::string>> title;
    if (const auto found = titles_.constFind(id); found != titles_.constEnd()) title = *found;
    // Fetched and decoded off the UI thread: a library's worth of covers
    // decoding here is what made the window stutter while they arrived.
    // Loaded from bytes, not a path: the image lives in mirad's own cache
    // directory, which the frontend has no business knowing.
    auto fetch = [id = id.toStdString(), title] {
      const ArtworkResult result = title ? MiradClient::GetTitleArtworkBlocking(title->first, title->second)
                                         : MiradClient::GetArtworkBlocking(id, "cover");
      QImage image;
      if (!result.ok || !image.loadFromData(reinterpret_cast<const uchar*>(result.bytes.data()),
                                            static_cast<int>(result.bytes.size()))) {
        return QImage();
      }
      // Never drawn bigger than the largest tile on a HiDPI screen; the rest is memory.
      if (image.width() > kMaxArt.width() || image.height() > kMaxArt.height()) {
        image = image.scaled(kMaxArt, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      }
      return image;
    };
    async::Run<QImage>(this, std::move(fetch), [this, id](QImage image) {
      --in_flight_;
      queued_.remove(id);
      // Answered covers all three outcomes on purpose: re-asking on every
      // repaint would turn an empty library into a request loop. Only
      // Invalidate reopens the question.
      answered_.insert(id);
      if (!image.isNull()) {
        original_.insert(id, QPixmap::fromImage(std::move(image)));
        InvalidateRendering(id.toStdString());
        emit CoverChanged(id);
      } else if (original_.remove(id) > 0) {
        // A refetch found it gone.
        InvalidateRendering(id.toStdString());
        emit CoverChanged(id);
      }
      if (ask_again_.remove(id)) {
        answered_.remove(id);
        Request(id);
      }
      Pump();
    });
  }
}

}  // namespace mira_gui
