#include "HoverCard.h"

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "GamePresentation.h"
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

}  // namespace

HoverCard::HoverCard(QWidget* parent) : QWidget(parent) {
  // ToolTip: never takes focus or activation, never shows in a taskbar --
  // exactly the non-modal, glance-and-gone behavior a hover preview wants.
  setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(14, 12, 14, 12);
  layout->setSpacing(4);
  setFixedWidth(240);

  name_ = new QLabel(this);
  name_->setProperty("role", "section");
  name_->setWordWrap(true);
  layout->addWidget(name_);

  status_ = new QLabel(this);
  status_->setTextFormat(Qt::RichText);
  layout->addWidget(status_);

  protondb_ = new QLabel(this);
  // Fixed, not the layout's default: a QSS-painted pill should hug its own
  // text, not stretch to the card's width like every other line here.
  protondb_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  protondb_->hide();
  layout->addWidget(protondb_);

  layout->addSpacing(4);

  platform_line_ = new QLabel(this);
  platform_line_->setProperty("role", "muted");
  platform_line_->setWordWrap(true);
  layout->addWidget(platform_line_);

  played_line_ = new QLabel(this);
  played_line_->setProperty("role", "muted");
  layout->addWidget(played_line_);

  developer_line_ = new QLabel(this);
  developer_line_->setProperty("role", "muted");
  developer_line_->setWordWrap(true);
  developer_line_->hide();
  layout->addWidget(developer_line_);
}

void HoverCard::ShowGame(const GameSummary& game, bool running) {
  game_id_ = game.id;

  name_->setText(QString::fromStdString(game.name));
  status_->setText(QString("<span style='color:%1; font-weight:600;'>%2</span>")
                        .arg(StatusColor(running ? "running" : game.status).name(),
                            running ? "Playing" : StatusLabel(game.status)));

  const bool native = game.platform == "native";
  QString platform_label = QString::fromStdString(game.platform);
  if (!platform_label.isEmpty()) platform_label[0] = platform_label[0].toUpper();
  platform_line_->setText(native ? "Native — no Proton involved"
                                 : QString("Platform: %1").arg(platform_label));
  played_line_->setText(QString("%1 · %2").arg(FormatLastPlayed(game.last_played_at),
                                               FormatPlaytime(game.play_seconds)));

  protondb_->hide();
  developer_line_->hide();
  adjustSize();

  MiradClient::GetMetadataAsync(this, game.id, [this, id = game.id](GameMetadataResult result) {
    if (game_id_ != id || !result.ok || result.missing) return;
    ShowMetadata(result.metadata);
  });
}

void HoverCard::ShowMetadata(const GameMetadata& metadata) {
  const QString tier = QString::fromStdString(metadata.protondb_tier);
  if (!tier.isEmpty()) {
    QString label = tier;
    label[0] = label[0].toUpper();
    const QColor background = ProtonDbTierColor(metadata.protondb_tier);
    // A real QSS-painted background, not rich-text HTML: Qt's rich text
    // engine only crudely rounds a span's background.
    protondb_->setText(label);
    protondb_->setStyleSheet(QString("QLabel { background-color: %1; color: %2; "
                                     "padding: 2px 10px; border-radius: 10px; font-weight: 700; }")
                                 .arg(background.name(), ContrastingTextColor(background).name()));
  }
  protondb_->setVisible(!tier.isEmpty());

  const std::string developer_genre = Join(metadata.developers) +
      (!metadata.developers.empty() && !metadata.genres.empty() ? " · " : "") + Join(metadata.genres);
  developer_line_->setText(QString::fromStdString(developer_genre));
  developer_line_->setVisible(!developer_genre.empty());

  adjustSize();
}

void HoverCard::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  QPainterPath card;
  card.addRoundedRect(rect().adjusted(0, 0, -1, -1), tokens.radius_panel, tokens.radius_panel);
  painter.fillPath(card, tokens.surface);
  painter.setPen(QPen(tokens.border, 1));
  painter.drawPath(card);
}

}  // namespace mira_gui
