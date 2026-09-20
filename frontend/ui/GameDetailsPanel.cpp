#include "GameDetailsPanel.h"

#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "AboutPanel.h"
#include "ArtworkStore.h"
#include "CoverArt.h"
#include "GamePresentation.h"
#include "Theme.h"

#include <algorithm>

namespace mira_gui {
namespace {

// The details panel's image area is hero-shaped: a game with a hero shows
// it, one without shows its cover instead, but the box itself — and how a
// mismatched source fits into it — doesn't change based on which. Scales
// `source` down or up so it fits entirely inside `box` without being
// cropped, then centers it on `background`. A null `source` just paints the
// empty box, for a placeholder cover that's already drawn to fill it.
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

std::string Join(const std::vector<std::string>& values) {
  std::string joined;
  for (const std::string& value : values) {
    if (!joined.empty()) joined += ", ";
    joined += value;
  }
  return joined;
}

// QPushButton's own default horizontal size policy is Fixed — one of these,
// with wording like "Refresh cover art & metadata", dictates a floor on the
// sidebar's minimum width the same way an unwrappable label does (see
// ValueLabel), and dragging the splitter narrower than that floor just left
// its own text spilling past the panel instead of shrinking. Ignored fixes
// it the same way: still full width when there's room, no longer a floor
// when there isn't.
QPushButton* ActionButton(const QString& text, QWidget* parent) {
  auto* button = new QPushButton(text, parent);
  button->setSizePolicy(QSizePolicy::Ignored, button->sizePolicy().verticalPolicy());
  return button;
}

QLabel* ValueLabel(QWidget* parent) {
  auto* label = new QLabel(parent);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label->setWordWrap(true);
  // Ignored, not Preferred: word wrap can't break an unbroken string (a
  // path, "proton:GE-Proton11-7"), so its minimumSizeHint was the sidebar's
  // own floor, wider than the splitter was ever dragged to.
  label->setSizePolicy(QSizePolicy::Ignored, label->sizePolicy().verticalPolicy());
  return label;
}

}  // namespace

GameDetailsPanel::GameDetailsPanel(QWidget* parent) : QWidget(parent) {
  stack_ = new QStackedWidget(this);

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(stack_);

  stack_->addWidget(new AboutPanel(stack_));

  auto* panel = new QWidget(stack_);
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(10);

  // Exactly one of these three is ever visible (UpdateImageVisibility):
  // hero, cover, or this placeholder while it's still unknown which — never
  // the cover speculatively, to avoid a cover-then-hero flash.
  // Ignored horizontally: a QLabel's sizeHint tracks its current pixmap, so
  // last render's width would floor the panel's minimum and block shrinking
  // to a smaller one. Same ratchet as ValueLabel's, for images not text.
  banner_ = new QLabel(panel);
  banner_->setObjectName("game_details_banner");
  banner_->setAlignment(Qt::AlignCenter);
  banner_->setSizePolicy(QSizePolicy::Ignored, banner_->sizePolicy().verticalPolicy());
  banner_->setVisible(false);
  layout->addWidget(banner_);

  cover_ = new QLabel(panel);
  cover_->setObjectName("game_details_cover");
  cover_->setAlignment(Qt::AlignCenter);
  cover_->setSizePolicy(QSizePolicy::Ignored, cover_->sizePolicy().verticalPolicy());
  cover_->setVisible(false);
  layout->addWidget(cover_);

  placeholder_ = new QLabel("No image yet", panel);
  placeholder_->setObjectName("game_details_placeholder");
  placeholder_->setAlignment(Qt::AlignCenter);
  placeholder_->setProperty("role", "muted");
  placeholder_->setFixedHeight(ImageBoxSize().height());
  layout->addWidget(placeholder_);

  name_ = new QLabel(panel);
  name_->setWordWrap(true);
  name_->setProperty("role", "heading");
  layout->addWidget(name_);

  status_ = new QLabel(panel);
  layout->addWidget(status_);

  play_ = ActionButton("Play", panel);
  play_->setMinimumHeight(34);
  connect(play_, &QPushButton::clicked, this, [this] {
    if (game_id_.empty()) return;
    const QString id = QString::fromStdString(game_id_);
    if (running_) {
      emit StopRequested(id);
    } else {
      emit PlayRequested(id);
    }
  });
  layout->addWidget(play_);

  auto* edit = ActionButton("Details && settings…", panel);
  connect(edit, &QPushButton::clicked, this, [this] {
    if (!game_id_.empty()) emit EditRequested(QString::fromStdString(game_id_));
  });
  layout->addWidget(edit);

  // Also on the tile's right-click menu, but a right-click menu is not
  // where anyone looks for "this game has the wrong picture".
  auto* refresh_metadata = ActionButton("Refresh cover art && metadata", panel);
  refresh_metadata->setToolTip(
      "Re-fetch this game's cover and store info. Worth doing after setting a SteamGridDB key, "
      "which is what a non-Steam game needs before it can have artwork at all.");
  connect(refresh_metadata, &QPushButton::clicked, this, [this] {
    if (!game_id_.empty()) emit MetadataRefreshRequested(QString::fromStdString(game_id_));
  });
  layout->addWidget(refresh_metadata);

  auto* choose_artwork = ActionButton("Choose cover art…", panel);
  choose_artwork->setToolTip(
      "Browse SteamGridDB's other results for this game's cover, if it has any cached.");
  connect(choose_artwork, &QPushButton::clicked, this, [this] {
    if (!game_id_.empty()) emit ArtworkPickRequested(QString::fromStdString(game_id_), "cover");
  });
  layout->addWidget(choose_artwork);

  auto* choose_hero = ActionButton("Choose hero art…", panel);
  choose_hero->setToolTip(
      "Browse SteamGridDB's other results for this game's wide banner art, if it has any cached.");
  connect(choose_hero, &QPushButton::clicked, this, [this] {
    if (!game_id_.empty()) emit ArtworkPickRequested(QString::fromStdString(game_id_), "hero");
  });
  layout->addWidget(choose_hero);

  description_ = ValueLabel(panel);
  description_->setProperty("role", "muted");
  description_->setVisible(false);
  layout->addWidget(description_);

  form_ = new QFormLayout();
  QFormLayout* form = form_;
  form->setLabelAlignment(Qt::AlignLeft);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  platform_ = ValueLabel(panel);
  runner_ = ValueLabel(panel);
  last_played_ = ValueLabel(panel);
  playtime_ = ValueLabel(panel);
  path_ = ValueLabel(panel);
  form->addRow("Platform", platform_);
  form->addRow("Runner", runner_);
  form->addRow("Last played", last_played_);
  form->addRow("Playtime", playtime_);
  form->addRow("Install path", path_);
  released_ = ValueLabel(panel);
  developer_ = ValueLabel(panel);
  genres_ = ValueLabel(panel);
  reviews_ = ValueLabel(panel);
  protondb_ = ValueLabel(panel);
  form->addRow("Released", released_);
  form->addRow("Developer", developer_);
  form->addRow("Genres", genres_);
  form->addRow("Reviews", reviews_);
  form->addRow("ProtonDB", protondb_);
  layout->addLayout(form);

  error_ = ValueLabel(panel);
  error_->setProperty("role", "error");
  error_->setVisible(false);
  layout->addWidget(error_);

  layout->addStretch(1);

  auto* scroll = new QScrollArea(stack_);
  scroll->setWidget(panel);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  stack_->addWidget(scroll);

  // The banner and cover are painted pixmaps, so the stylesheet cannot
  // re-round or resize them itself when the corner radius or hero_height
  // changes.
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
    RenderBanner();
    placeholder_->setFixedHeight(ImageBoxSize().height());
    if (current_game_) RefreshCover(*current_game_);
  });

  Clear();
}

void GameDetailsPanel::Clear() {
  game_id_.clear();
  current_game_.reset();
  stack_->setCurrentIndex(0);
}

void GameDetailsPanel::ClearMetadata() {
  description_->setVisible(false);
  banner_source_ = QPixmap();
  for (QLabel* label : {released_, developer_, genres_, reviews_, protondb_}) {
    form_->setRowVisible(label, false);
  }
}

void GameDetailsPanel::ShowMetadata(const GameMetadata& metadata) {
  const auto row = [this](QLabel* label, const QString& text) {
    label->setText(text);
    form_->setRowVisible(label, !text.isEmpty());
  };

  description_->setText(QString::fromStdString(metadata.description));
  description_->setVisible(!metadata.description.empty());

  row(released_, QString::fromStdString(metadata.release_date));
  row(developer_, QString::fromStdString(Join(metadata.developers)));
  row(genres_, QString::fromStdString(Join(metadata.genres)));

  QString reviews = QString::fromStdString(metadata.review_summary);
  if (!reviews.isEmpty() && metadata.review_total > 0) {
    reviews += QString(" (%L1 reviews)").arg(metadata.review_total);
  }
  if (metadata.metacritic_score > 0) {
    const QString metacritic = QString("Metacritic %1").arg(metadata.metacritic_score);
    reviews = reviews.isEmpty() ? metacritic : reviews + " · " + metacritic;
  }
  row(reviews_, reviews);

  // ProtonDB's own wording, capitalized: "platinum" is a tier name, not a
  // sentence, and its meaning is the site's rather than ours to restate.
  QString tier = QString::fromStdString(metadata.protondb_tier);
  if (!tier.isEmpty()) tier[0] = tier[0].toUpper();
  row(protondb_, tier);
}

void GameDetailsPanel::LoadMetadata(const std::string& id) {
  MiradClient::GetMetadataAsync(this, id, [this, id](GameMetadataResult result) {
    if (game_id_ != id) return;  // the selection moved on while this was in flight
    const QString key = QString::fromStdString(id);
    if (!result.ok) {
      // Missing is the ordinary case and says nothing to show, but
      // hero_known_ still needs an answer -- otherwise placeholder_ (shown
      // while ShowGame doesn't know yet) would never resolve to the cover.
      hero_known_[key] = false;
      UpdateImageVisibility();
      return;
    }
    ShowMetadata(result.metadata);
    // Not named `slots`: Qt's moc keywords define that as a macro.
    const std::vector<std::string>& art = result.metadata.art_slots;
    const bool has_hero = std::find(art.begin(), art.end(), "hero") != art.end();
    hero_known_[key] = has_hero;
    if (has_hero) LoadBanner(id);
    UpdateImageVisibility();
  });
}

void GameDetailsPanel::LoadBanner(const std::string& id) {
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

QSize GameDetailsPanel::ImageBoxSize() const {
  // The parent's width, not banner_'s own: banner_ starts hidden and never
  // laid out, so its width can read stale/default while the panel around it
  // is already the sidebar's real width. 24 is the panel's own margins.
  const int width = qMax(120, banner_->parentWidget()->width() - 24);
  // Height derived from width via SteamGridDB's own 1920x620 ratio, not
  // just hero_height outright — a box whose shape doesn't match a real
  // hero's pads even a correctly-sized one with empty bands. hero_height
  // now only caps how tall that gets on a wide sidebar.
  constexpr qreal kHeroAspect = 1920.0 / 620.0;
  const int height = qMax(20, qMin(theme::Current().hero_height, qRound(width / kHeroAspect)));
  return QSize(width, height);
}

void GameDetailsPanel::RenderBanner() {
  if (banner_source_.isNull()) return;
  // Contain-fit, not cover: SteamGridDB's own hero shape is 1920x620, but a
  // mismatched alternate should show whole and undistorted, not have its
  // edges cut off — same treatment RefreshCover gives a game with no hero.
  banner_->setPixmap(FitLetterboxed(banner_source_, ImageBoxSize(), theme::Current().radius_panel,
                                    theme::Current().surface_alt));
  UpdateImageVisibility();
}

void GameDetailsPanel::UpdateImageVisibility() {
  const QString key = QString::fromStdString(game_id_);
  const auto known = hero_known_.constFind(key);
  const bool confirmed_no_hero = known != hero_known_.constEnd() && !known.value();
  const bool hero_ready = known != hero_known_.constEnd() && known.value() && banners_.contains(key);

  banner_->setVisible(hero_ready);
  cover_->setVisible(confirmed_no_hero);
  placeholder_->setVisible(!hero_ready && !confirmed_no_hero);
}

void GameDetailsPanel::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  RenderBanner();
  // Kept in sync even while hidden, or its pixmap stays sized for whatever
  // width the panel had the last time it was actually the visible one.
  if (current_game_) RefreshCover(*current_game_);
  // ImageBoxSize()'s height now tracks width (see its own comment), so the
  // placeholder's fixed height is just as stale on a resize as the other
  // two's pixmaps are.
  placeholder_->setFixedHeight(ImageBoxSize().height());
}

void GameDetailsPanel::SetArtworkStore(ArtworkStore* store) { artwork_ = store; }

void GameDetailsPanel::RefreshBanner(const std::string& id) {
  banners_.remove(QString::fromStdString(id));
  if (game_id_ == id) LoadBanner(id);
}

void GameDetailsPanel::RefreshCover(const GameSummary& game) {
  const QSize box = ImageBoxSize();
  const QString name = QString::fromStdString(game.name);
  const QString id = QString::fromStdString(game.id);

  if (artwork_ != nullptr) artwork_->EnsureRequested(game.id);
  if (artwork_ != nullptr && artwork_->HasArtwork(game.id)) {
    // Real art: fit-and-letterbox it into the same box the hero banner
    // gets, rather than the cover-crop Cover() itself would give a grid
    // tile — a cover showing here at all means there's no hero, and this
    // box is hero-shaped either way; never cropped, same as RenderBanner.
    cover_->setPixmap(FitLetterboxed(artwork_->RawArtwork(game.id), box,
                                     theme::Current().radius_panel, theme::Current().surface_alt));
    return;
  }
  // No real art (yet, or ever) — the generated placeholder is drawn
  // straight to the box's own size; nothing to letterbox since it's
  // synthetic, not sourced from an image with its own aspect ratio.
  cover_->setPixmap(PlaceholderCover(name, id, box, devicePixelRatioF()));
}

void GameDetailsPanel::ShowGame(const GameSummary& game, bool running) {
  const bool same_game = game_id_ == game.id;
  game_id_ = game.id;
  running_ = running;
  current_game_ = game;
  stack_->setCurrentIndex(1);

  // Applied immediately, before the metadata round trip below even starts —
  // a repeat selection skips the placeholder and any cover-then-hero flash.
  UpdateImageVisibility();

  // Only on a real change of selection: ShowGame also runs for a state event
  // on the game already shown, and clearing there would flicker.
  if (!same_game) {
    ClearMetadata();
    LoadMetadata(game.id);
  }

  RefreshCover(game);
  name_->setText(QString::fromStdString(game.name));

  // "Ready" is the common case and says nothing worth a line of its own —
  // only a state that needs attention (or Playing) earns one.
  const bool can_launch = game.status == "ready";
  status_->setVisible(running || !can_launch);
  status_->setText(running ? "Playing now" : StatusLabel(game.status));
  theme::SetStyleProperty(status_, "status",
                          running ? QString("running") : QString::fromStdString(game.status));

  play_->setText(running ? "Stop" : "Play");
  play_->setEnabled(running || can_launch);
  play_->setToolTip(running || can_launch
                        ? QString()
                        : QString("Not launchable while %1").arg(StatusLabel(game.status).toLower()));

  platform_->setText(QString::fromStdString(game.platform));
  runner_->setText(game.runner_ref.empty() ? "Auto (use default runner)"
                                           : QString::fromStdString(game.runner_ref));
  last_played_->setText(FormatLastPlayed(game.last_played_at));
  playtime_->setText(FormatPlaytime(game.play_seconds));

  error_->setVisible(!game.last_error.empty());
  error_->setText(QString::fromStdString(game.last_error));

  path_->setText(QString::fromStdString(game.install_path));
  path_->setToolTip(path_->text());
}

}  // namespace mira_gui
