#include "HeroBackdrop.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

#include "../client/MiradClient.h"
#include "ArtworkStore.h"
#include "CoverArt.h"
#include "Theme.h"

namespace mira_gui {
namespace {

// Share of the card's height the art covers before it has faded out.
constexpr qreal kBandShare = 0.6;

}  // namespace

HeroBackdrop::HeroBackdrop(ArtworkStore* artwork, QWidget* parent) : QWidget(parent), artwork_(artwork) {
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] { update(); });
}

void HeroBackdrop::ShowGame(const GameSummary& game) {
  game_ = game;
  hero_ = QPixmap();
  rendered_ = QPixmap();
  if (artwork_ != nullptr) artwork_->EnsureRequested(game.id);
  LoadHero();
  update();
}

void HeroBackdrop::RefreshCover(const std::string& id) {
  if (id != game_.id) return;
  rendered_ = QPixmap();
  update();
}

void HeroBackdrop::RefreshHero(const std::string& id) {
  if (id != game_.id) return;
  LoadHero();
}

void HeroBackdrop::LoadHero() {
  const std::string id = game_.id;
  MiradClient::GetMetadataAsync(this, id, [this, id](GameMetadataResult result) {
    if (game_.id != id || !result.ok) return;
    const std::vector<std::string>& art_slots = result.metadata.art_slots;
    if (std::ranges::find(art_slots, "hero") == art_slots.end()) return;
    MiradClient::GetArtworkSlotAsync(this, id, "hero", [this, id](ArtworkResult art) {
      if (game_.id != id || !art.ok) return;
      QPixmap pixmap;
      if (!pixmap.loadFromData(reinterpret_cast<const uchar*>(art.bytes.data()),
                               static_cast<uint>(art.bytes.size()))) {
        return;
      }
      hero_ = pixmap;
      rendered_ = QPixmap();
      update();
    });
  });
}

QPixmap HeroBackdrop::Source() const {
  if (!hero_.isNull()) return hero_;
  if (artwork_ != nullptr && artwork_->HasArtwork(game_.id)) return artwork_->RawArtwork(game_.id);
  // The same generated art as its tile, so the card still carries its colors.
  return PlaceholderCover(QString::fromStdString(game_.name), QString::fromStdString(game_.id), QSize(200, 300), 1);
}

void HeroBackdrop::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  QPainterPath card;
  card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), tokens.radius_panel, tokens.radius_panel);
  painter.fillPath(card, tokens.surface);

  const QPixmap source = Source();
  if (!source.isNull()) {
    const QRect band(0, 0, width(), qRound(height() * kBandShare));
    const qreal dpr = devicePixelRatioF();
    const qint64 key = source.cacheKey() ^ (qint64(band.width()) << 32) ^ band.height();
    if (rendered_.isNull() || rendered_key_ != key) {
      QPixmap scaled = source.scaled(band.size() * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
      // A cover is portrait: a slice of it reads as noise, so blur it into
      // a wash of its colors (down to a few pixels and back up).
      if (hero_.isNull()) {
        scaled = scaled.scaled(scaled.size() / 64, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                     .scaled(scaled.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
      }
      scaled.setDevicePixelRatio(dpr);
      rendered_ = scaled;
      rendered_key_ = key;
    }
    const QSizeF size = rendered_.deviceIndependentSize();
    painter.save();
    painter.setClipPath(card);
    painter.setClipRect(band, Qt::IntersectClip);
    painter.drawPixmap(QPointF((band.width() - size.width()) / 2, (band.height() - size.height()) / 2),
                       rendered_);
    // Fades into the surface, so text laid over it keeps the theme's colors.
    QLinearGradient fade(0, 0, 0, band.height());
    QColor surface = tokens.surface;
    surface.setAlphaF(0.2);
    fade.setColorAt(0, surface);
    surface.setAlphaF(0.75);
    fade.setColorAt(0.55, surface);
    surface.setAlphaF(1);
    fade.setColorAt(1, surface);
    painter.fillRect(band, fade);
    painter.restore();
  }

  painter.setPen(QPen(tokens.border, 1));
  painter.drawPath(card);
}

}  // namespace mira_gui
