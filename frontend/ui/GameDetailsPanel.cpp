#include "GameDetailsPanel.h"

#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "AboutPanel.h"
#include "ArtworkStore.h"
#include "GamePresentation.h"
#include "HeroArtWidget.h"
#include "Theme.h"

namespace mira_gui {
namespace {

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

  hero_art_ = new HeroArtWidget(panel);
  layout->addWidget(hero_art_);

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

  // Grouped with the other action buttons, not stranded below the form.
  auto* more_details = ActionButton("More details…", panel);
  connect(more_details, &QPushButton::clicked, this, [this] {
    if (!game_id_.empty()) emit MoreDetailsRequested(QString::fromStdString(game_id_));
  });
  layout->addWidget(more_details);

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
  // A colored pill, not a plain value: fixed size policy so it hugs its own
  // text instead of stretching across the row like every other value here.
  protondb_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
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

  Clear();
}

void GameDetailsPanel::Clear() {
  game_id_.clear();
  hero_art_->Clear();
  stack_->setCurrentIndex(0);
}

void GameDetailsPanel::ClearMetadata() {
  description_->setVisible(false);
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
  // sentence, and its meaning is the site's rather than ours to restate. A
  // colored pill (see ProtonDbTierColor) reads faster than plain text next
  // to every other plain-text row here.
  QString tier = QString::fromStdString(metadata.protondb_tier);
  if (!tier.isEmpty()) {
    tier[0] = tier[0].toUpper();
    const QColor background = ProtonDbTierColor(metadata.protondb_tier);
    protondb_->setStyleSheet(QString("QLabel { background-color: %1; color: %2; "
                                     "padding: 2px 10px; border-radius: %3px; font-weight: 600; }")
                                 .arg(background.name(), theme::Current().on_accent.name())
                                 .arg(theme::Current().radius_control));
  }
  row(protondb_, tier);
}

void GameDetailsPanel::LoadMetadata(const std::string& id) {
  MiradClient::GetMetadataAsync(this, id, [this, id](GameMetadataResult result) {
    if (game_id_ != id) return;  // the selection moved on while this was in flight
    if (result.ok) ShowMetadata(result.metadata);
  });
}

void GameDetailsPanel::SetArtworkStore(ArtworkStore* store) { hero_art_->SetArtworkStore(store); }

void GameDetailsPanel::RefreshBanner(const std::string& id) { hero_art_->RefreshBanner(id); }

void GameDetailsPanel::RefreshCover(const GameSummary&) { hero_art_->RefreshCover(); }

void GameDetailsPanel::ShowGame(const GameSummary& game, bool running) {
  const bool same_game = game_id_ == game.id;
  game_id_ = game.id;
  running_ = running;
  stack_->setCurrentIndex(1);
  hero_art_->ShowGame(game);

  // Only on a real change of selection: ShowGame also runs for a state event
  // on the game already shown, and clearing there would flicker.
  if (!same_game) {
    ClearMetadata();
    LoadMetadata(game.id);
  }

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
