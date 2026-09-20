#include "HeroArtWidget.h"

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "ArtworkStore.h"
#include "CoverArt.h"
#include "Theme.h"

#include <algorithm>

namespace mira_gui {
namespace {

// This box is hero-shaped: a game with a hero shows it, one without shows
// its cover instead, but the box itself — and how a mismatched source fits
// into it — doesn't change based on which. Scales `source` down or up so it
// fits entirely inside `box` without being cropped, then centers it on
// `background`. A null `source` just paints the empty box, for a
// placeholder cover that's already drawn to fill it.
QPixmap FitLetterboxed(const QPixmap& source, QSize box, int radius, const QColor& background) {
  QPixmap canvas(box);
  canvas.fill(Qt::transparent);
  QPainter painter(&canvas);
  painter.setRenderHint(QPainter::Antialiasing);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(0, 0, box.width(), box.height()), radius, radius);
  painter.setClipPath(clip);
  painter.fillRect(QRectF(0, 0, box.width(), box.height()), background);

  if (!source.isNull()) {
    const qreal scale = qMin(static_cast<qreal>(box.width()) / source.width(),
                             static_cast<qreal>(box.height()) / source.height());
    const QSize fitted_size(qMax(1, qRound(source.width() * scale)),
                            qMax(1, qRound(source.height() * scale)));
    const QPixmap fitted =
        source.scaled(fitted_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    painter.drawPixmap((box.width() - fitted.width()) / 2, (box.height() - fitted.height()) / 2,
                       fitted);
  }
  painter.end();
  return canvas;
}

}  // namespace

HeroArtWidget::HeroArtWidget(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  // Exactly one of these three is ever visible (UpdateImageVisibility):
  // hero, cover, or this placeholder while it's still unknown which — never
  // the cover speculatively, to avoid a cover-then-hero flash.
  // Ignored horizontally: a QLabel's sizeHint tracks its current pixmap, so
  // last render's width would floor this widget's minimum and block
  // shrinking to a smaller one.
  banner_ = new QLabel(this);
  banner_->setObjectName("game_details_banner");
  banner_->setAlignment(Qt::AlignCenter);
  banner_->setSizePolicy(QSizePolicy::Ignored, banner_->sizePolicy().verticalPolicy());
  banner_->setVisible(false);
  layout->addWidget(banner_);

  cover_ = new QLabel(this);
  cover_->setObjectName("game_details_cover");
  cover_->setAlignment(Qt::AlignCenter);
  cover_->setSizePolicy(QSizePolicy::Ignored, cover_->sizePolicy().verticalPolicy());
  cover_->setVisible(false);
  layout->addWidget(cover_);

  placeholder_ = new QLabel("No image yet", this);
  placeholder_->setObjectName("game_details_placeholder");
  placeholder_->setAlignment(Qt::AlignCenter);
  placeholder_->setProperty("role", "muted");
  placeholder_->setFixedHeight(ImageBoxSize().height());
  layout->addWidget(placeholder_);

  // The banner and cover are painted pixmaps, so the stylesheet cannot
  // re-round or resize them itself when the corner radius or hero_height
  // changes.
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
    RenderBanner();
    placeholder_->setFixedHeight(ImageBoxSize().height());
    RefreshCover();
  });
}

void HeroArtWidget::SetArtworkStore(ArtworkStore* store) { artwork_ = store; }

void HeroArtWidget::Clear() {
  game_id_.clear();
  current_game_.reset();
  banner_source_ = QPixmap();
  UpdateImageVisibility();
}

void HeroArtWidget::ShowGame(const GameSummary& game) {
  const bool same_game = game_id_ == game.id;
  game_id_ = game.id;
  current_game_ = game;

  // Applied immediately, before the metadata round trip below even starts —
  // a repeat selection skips the placeholder and any cover-then-hero flash.
  UpdateImageVisibility();
  RefreshCover();

  if (!same_game) LoadMetadata(game.id);
}

void HeroArtWidget::LoadMetadata(const std::string& id) {
  MiradClient::GetMetadataAsync(this, id, [this, id](GameMetadataResult result) {
    if (game_id_ != id) return;  // the selection moved on while this was in flight
    const QString key = QString::fromStdString(id);
    // Missing is the ordinary case and says nothing to show, but
    // hero_known_ still needs an answer -- otherwise placeholder_ (shown
    // while ShowGame doesn't know yet) would never resolve to the cover.
    // Not named `slots`: Qt's moc keywords define that as a macro.
    const bool has_hero = result.ok && std::find(result.metadata.art_slots.begin(),
                                                  result.metadata.art_slots.end(),
                                                  "hero") != result.metadata.art_slots.end();
    hero_known_[key] = has_hero;
    if (has_hero) LoadBanner(id);
    UpdateImageVisibility();
  });
}

void HeroArtWidget::LoadBanner(const std::string& id) {
  const QString key = QString::fromStdString(id);
  if (banners_.contains(key)) {
    banner_source_ = banners_.value(key);
    RenderBanner();
    return;
  }
  MiradClient::GetArtworkSlotAsync(this, id, "hero", [this, id, key](ArtworkResult result) {
    if (!result.ok) return;
    QPixmap pixmap;
    if (!pixmap.loadFromData(reinterpret_cast<const uchar*>(result.bytes.data()),
                             static_cast<uint>(result.bytes.size()))) {
      return;
    }
    banners_.insert(key, pixmap);
    if (game_id_ != id) return;
    banner_source_ = pixmap;
    RenderBanner();
  });
}

QSize HeroArtWidget::ImageBoxSize() const {
  // This widget's own width, not banner_'s: banner_ starts hidden and never
  // laid out, so its width can read stale/default while this widget is
  // already the sidebar's real width.
  const int width = qMax(120, this->width());
  // Height derived from width via SteamGridDB's own 1920x620 ratio, not
  // just hero_height outright — a box whose shape doesn't match a real
  // hero's pads even a correctly-sized one with empty bands. hero_height
  // now only caps how tall that gets on a wide sidebar.
  constexpr qreal kHeroAspect = 1920.0 / 620.0;
  const int height = qMax(20, qMin(theme::Current().hero_height, qRound(width / kHeroAspect)));
  return QSize(width, height);
}

void HeroArtWidget::RenderBanner() {
  if (banner_source_.isNull()) return;
  // Contain-fit, not cover: SteamGridDB's own hero shape is 1920x620, but a
  // mismatched alternate should show whole and undistorted, not have its
  // edges cut off — same treatment RefreshCover gives a game with no hero.
  banner_->setPixmap(FitLetterboxed(banner_source_, ImageBoxSize(), theme::Current().radius_panel,
                                    theme::Current().surface_alt));
  UpdateImageVisibility();
}

void HeroArtWidget::UpdateImageVisibility() {
  const QString key = QString::fromStdString(game_id_);
  const auto known = hero_known_.constFind(key);
  const bool confirmed_no_hero = known != hero_known_.constEnd() && !known.value();
  const bool hero_ready = known != hero_known_.constEnd() && known.value() && banners_.contains(key);

  banner_->setVisible(hero_ready);
  cover_->setVisible(confirmed_no_hero);
  placeholder_->setVisible(!hero_ready && !confirmed_no_hero);
}

void HeroArtWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  RenderBanner();
  RefreshCover();
  // ImageBoxSize()'s height tracks width, so the placeholder's fixed height
  // is just as stale on a resize as the other two's pixmaps are.
  placeholder_->setFixedHeight(ImageBoxSize().height());
}

void HeroArtWidget::RefreshBanner(const std::string& id) {
  banners_.remove(QString::fromStdString(id));
  if (game_id_ == id) LoadBanner(id);
}

void HeroArtWidget::RefreshCover() {
  if (!current_game_) return;
  const QSize box = ImageBoxSize();
  const QString name = QString::fromStdString(current_game_->name);
  const QString id = QString::fromStdString(current_game_->id);

  if (artwork_ != nullptr) artwork_->EnsureRequested(current_game_->id);
  if (artwork_ != nullptr && artwork_->HasArtwork(current_game_->id)) {
    // Real art: fit-and-letterbox it into the same box the hero banner
    // gets, rather than the cover-crop Cover() itself would give a grid
    // tile — a cover showing here at all means there's no hero, and this
    // box is hero-shaped either way; never cropped, same as RenderBanner.
    cover_->setPixmap(FitLetterboxed(artwork_->RawArtwork(current_game_->id), box,
                                     theme::Current().radius_panel, theme::Current().surface_alt));
    return;
  }
  // No real art (yet, or ever) — the generated placeholder is drawn
  // straight to the box's own size; nothing to letterbox since it's
  // synthetic, not sourced from an image with its own aspect ratio.
  cover_->setPixmap(PlaceholderCover(name, id, box, devicePixelRatioF()));
}

}  // namespace mira_gui
