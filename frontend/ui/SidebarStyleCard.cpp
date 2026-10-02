#include "SidebarStyleCard.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

#include "ArtworkStore.h"
#include "GamePresentation.h"
#include "Icons.h"
#include "Theme.h"

namespace mira_gui {
namespace {

// A tile previewing one style above its name.
class StyleChoice : public QAbstractButton {
public:
  StyleChoice(const sidebar::StyleOption& option, std::function<std::vector<sidebar::PreviewGame>()> games,
              ArtworkStore* artwork, QWidget* parent)
      : QAbstractButton(parent), option_(option), games_(std::move(games)), artwork_(artwork) {
    setCheckable(true);
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(option.label);
    setFixedSize(176, 132);
  }

protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& tokens = theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
    painter.setPen(QPen(isChecked() ? tokens.accent : tokens.border, isChecked() ? 2 : 1));
    painter.setBrush(isChecked() || underMouse() ? tokens.surface_alt : tokens.surface);
    painter.drawRoundedRect(frame, tokens.radius_control + 2, tokens.radius_control + 2);
    if (hasFocus()) {
      painter.setPen(QPen(tokens.accent, 1, Qt::DotLine));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(frame.adjusted(3, 3, -3, -3), tokens.radius_control, tokens.radius_control);
    }

    sidebar::PaintStylePreview(&painter, QRect(12, 12, width() - 24, 86), option_.style, games_(), artwork_);

    QFont label = font();
    label.setWeight(QFont::DemiBold);
    painter.setFont(label);
    painter.setPen(tokens.text);
    painter.drawText(QRect(12, 106, width() - 24, 18), Qt::AlignLeft | Qt::AlignVCenter, option_.label);
  }

private:
  sidebar::StyleOption option_;
  std::function<std::vector<sidebar::PreviewGame>()> games_;
  ArtworkStore* artwork_;
};

QLabel* Heading(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", "section");
  return label;
}

}  // namespace

SidebarStyleCard::SidebarStyleCard(const Choices& choices, std::vector<GameSummary> pinned,
                                   std::vector<GameSummary> recent, ArtworkStore* artwork, QWidget* parent)
    : QFrame(parent), choices_(choices), pinned_(std::move(pinned)), recent_(std::move(recent)), artwork_(artwork) {
  setObjectName("sidebar_style_card");
  // Art that lands while the card is open shows in its previews.
  const auto repaint = [this] {
    for (QWidget* tile : findChildren<QAbstractButton*>()) tile->update();
  };
  connect(artwork_, &ArtworkStore::CoverChanged, this, repaint);
  connect(artwork_, &ArtworkStore::SlotArtChanged, this, repaint);
  connect(this, &SidebarStyleCard::Changed, this, repaint);  // the count and the when toggle
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(20, 16, 16, 20);
  layout->setSpacing(10);

  auto* header = new QHBoxLayout();
  auto* title = new QLabel("Pinned and recently played", this);
  title->setProperty("role", "heading");
  header->addWidget(title, /*stretch=*/1);
  auto* close = new QToolButton(this);
  close->setAutoRaise(true);
  close->setIcon(icons::For(icons::Glyph::Close));
  close->setToolTip("Close");
  connect(close, &QToolButton::clicked, this, &SidebarStyleCard::CloseRequested);
  header->addWidget(close);
  layout->addLayout(header);

  layout->addWidget(Heading("Pinned", this));
  layout->addWidget(MakeChoices(&choices_.pinned, /*recent=*/false));

  layout->addSpacing(8);
  layout->addWidget(Heading("Recently played", this));
  layout->addWidget(MakeChoices(&choices_.recent, /*recent=*/true));

  when_ = new QCheckBox("Show when games were last played", this);
  when_->setChecked(choices_.recent_when);
  connect(when_, &QCheckBox::toggled, this, [this](bool on) {
    choices_.recent_when = on;
    emit Changed(choices_);
  });
  layout->addWidget(when_);

  auto* count_row = new QHBoxLayout();
  count_row->addWidget(new QLabel("Games to show", this), /*stretch=*/1);
  recent_count_ = new QSpinBox(this);
  recent_count_->setRange(0, 10);
  recent_count_->setSpecialValueText("Off");
  recent_count_->setValue(choices_.recent_count);
  recent_count_->setToolTip("Running games always show.");
  connect(recent_count_, &QSpinBox::valueChanged, this, [this](int count) {
    choices_.recent_count = count;
    emit Changed(choices_);
  });
  count_row->addWidget(recent_count_);
  layout->addLayout(count_row);
}

std::vector<sidebar::PreviewGame> SidebarStyleCard::PreviewGames(bool recent, sidebar::Style style) const {
  std::vector<sidebar::PreviewGame> games;
  // Running games always show, the rest up to the count; on a shelf the running ones are part of it.
  int played = 0;
  if (recent && style == sidebar::Style::Shelf) {
    played = static_cast<int>(std::ranges::count(recent_, true, &GameSummary::running));
  }
  for (const GameSummary& game : recent ? recent_ : pinned_) {
    QString trailing;
    QString short_trailing;
    if (game.running) {
      trailing = short_trailing = "Playing";
    } else if (recent) {
      if (played++ >= choices_.recent_count) continue;
      if (choices_.recent_when) {
        trailing = FormatPlayedAgo(game.last_played_at);
        short_trailing = FormatPlayedAgoShort(game.last_played_at);
      }
    }
    games.push_back({game.id, QString::fromStdString(game.name), trailing, short_trailing});
  }
  return games;
}

QWidget* SidebarStyleCard::MakeChoices(sidebar::Style* target, bool recent) {
  auto* box = new QWidget(this);
  auto* row = new QHBoxLayout(box);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(10);
  auto* group = new QButtonGroup(box);
  for (const sidebar::StyleOption& option : sidebar::StyleOptions()) {
    const sidebar::Style shown = option.style;
    auto* choice = new StyleChoice(option, [this, recent, shown] { return PreviewGames(recent, shown); }, artwork_, box);
    choice->setChecked(option.style == *target);
    const sidebar::Style style = option.style;
    connect(choice, &QAbstractButton::clicked, this, [this, target, style] {
      *target = style;
      emit Changed(choices_);
    });
    group->addButton(choice);
    row->addWidget(choice);
  }
  row->addStretch(1);
  return box;
}

}  // namespace mira_gui
