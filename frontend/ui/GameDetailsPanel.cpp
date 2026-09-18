#include "GameDetailsPanel.h"

#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "ArtworkStore.h"
#include "CoverArt.h"
#include "GamePresentation.h"

namespace mira_gui {
namespace {

constexpr QSize kCoverSize(140, 210);

QLabel* ValueLabel(QWidget* parent) {
  auto* label = new QLabel(parent);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label->setWordWrap(true);
  return label;
}

}  // namespace

GameDetailsPanel::GameDetailsPanel(QWidget* parent) : QWidget(parent) {
  stack_ = new QStackedWidget(this);

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(stack_);

  auto* placeholder = new QLabel("Select a game to see its details.", stack_);
  placeholder->setAlignment(Qt::AlignCenter);
  placeholder->setWordWrap(true);
  placeholder->setStyleSheet("color: #9e9e9e;");
  stack_->addWidget(placeholder);

  auto* panel = new QWidget(stack_);
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(10);

  cover_ = new QLabel(panel);
  cover_->setAlignment(Qt::AlignCenter);
  layout->addWidget(cover_);

  name_ = new QLabel(panel);
  name_->setWordWrap(true);
  name_->setStyleSheet("font-size: 16px; font-weight: 600;");
  layout->addWidget(name_);

  status_ = new QLabel(panel);
  status_->setStyleSheet("font-size: 11px; font-weight: 600;");
  layout->addWidget(status_);

  play_ = new QPushButton("Play", panel);
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

  auto* edit = new QPushButton("Details && settings…", panel);
  connect(edit, &QPushButton::clicked, this, [this] {
    if (!game_id_.empty()) emit EditRequested(QString::fromStdString(game_id_));
  });
  layout->addWidget(edit);

  // Also on the tile's right-click menu, but a right-click menu is not
  // where anyone looks for "this game has the wrong picture".
  auto* refresh_metadata = new QPushButton("Refresh cover art && metadata", panel);
  refresh_metadata->setToolTip(
      "Re-fetch this game's cover and store info. Worth doing after setting a SteamGridDB key, "
      "which is what a non-Steam game needs before it can have artwork at all.");
  connect(refresh_metadata, &QPushButton::clicked, this, [this] {
    if (!game_id_.empty()) emit MetadataRefreshRequested(QString::fromStdString(game_id_));
  });
  layout->addWidget(refresh_metadata);

  auto* form = new QFormLayout();
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
  layout->addLayout(form);

  error_ = ValueLabel(panel);
  error_->setStyleSheet("color: #c62828; font-size: 11px;");
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
  stack_->setCurrentIndex(0);
}

void GameDetailsPanel::SetArtworkStore(ArtworkStore* store) { artwork_ = store; }

void GameDetailsPanel::RefreshCover(const GameSummary& game) {
  cover_->setPixmap(artwork_ != nullptr
                        ? artwork_->Cover(game, kCoverSize, devicePixelRatioF())
                        : PlaceholderCover(QString::fromStdString(game.name),
                                           QString::fromStdString(game.id), kCoverSize,
                                           devicePixelRatioF()));
}

void GameDetailsPanel::ShowGame(const GameSummary& game, bool running) {
  game_id_ = game.id;
  running_ = running;
  stack_->setCurrentIndex(1);

  RefreshCover(game);
  name_->setText(QString::fromStdString(game.name));

  // "Ready" is the common case and says nothing worth a line of its own —
  // only a state that needs attention (or Playing) earns one.
  const bool can_launch = game.status == "ready";
  status_->setVisible(running || !can_launch);
  status_->setText(running ? "Playing now" : StatusLabel(game.status));
  status_->setStyleSheet(QString("font-size: 11px; font-weight: 600; color: %1;")
                             .arg(running ? QString("#43a047") : StatusColor(game.status).name()));

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
