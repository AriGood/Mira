#include "GameTileDelegate.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include "GamePresentation.h"

namespace mira_gui {
namespace {

constexpr int kMargin = 5;
constexpr int kScrimHeight = 62;

}  // namespace

GameTileDelegate::GameTileDelegate(QObject* parent, QSize tile)
    : QStyledItemDelegate(parent), tile_(tile) {}

void GameTileDelegate::SetTileSize(QSize tile) { tile_ = tile; }

QSize GameTileDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
  return tile_;
}

void GameTileDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const {
  const QRect rect = option.rect.adjusted(kMargin, kMargin, -kMargin, -kMargin);
  if (rect.isEmpty()) return;

  const QString name = index.data(NameRole).toString();
  const std::string status = index.data(StatusRole).toString().toStdString();
  const bool running = index.data(RunningRole).toBool();
  const bool selected = option.state & QStyle::State_Selected;
  const bool hovered = option.state & QStyle::State_MouseOver;

  QPainterPath path;
  path.addRoundedRect(rect, 8, 8);

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing);
  painter->setClipPath(path);

  const QPixmap cover = index.data(Qt::DecorationRole).value<QPixmap>();
  if (cover.isNull()) {
    painter->fillRect(rect, QColor("#303030"));
  } else {
    painter->drawPixmap(rect, cover);
  }

  // The scrim exists so a light cover can't swallow the title — it is drawn
  // regardless of how dark the artwork underneath happens to be.
  const int scrim_height = qMin(rect.height(), kScrimHeight);
  const QRect scrim(rect.left(), rect.bottom() - scrim_height, rect.width(), scrim_height);
  QLinearGradient gradient(scrim.bottomLeft(), scrim.topLeft());
  gradient.setColorAt(0.0, QColor(0, 0, 0, 225));
  gradient.setColorAt(1.0, QColor(0, 0, 0, 0));
  painter->fillRect(scrim, gradient);
  painter->restore();

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing);

  QFont title_font = option.font;
  title_font.setWeight(QFont::DemiBold);
  painter->setFont(title_font);
  painter->setPen(QColor(255, 255, 255, 235));
  const QRect title_rect(rect.left() + 8, rect.bottom() - 36, rect.width() - 16, 18);
  painter->drawText(title_rect, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(title_font).elidedText(name, Qt::ElideRight, title_rect.width()));

  QFont status_font = option.font;
  status_font.setPixelSize(qMax(9, status_font.pixelSize() > 0 ? status_font.pixelSize() - 2 : 10));
  painter->setFont(status_font);
  const QRect status_rect(rect.left() + 8, rect.bottom() - 19, rect.width() - 16, 15);
  painter->setPen(Qt::NoPen);
  painter->setBrush(StatusColor(status).lighter(160));
  painter->drawEllipse(QPoint(status_rect.left() + 3, status_rect.center().y()), 3, 3);
  painter->setPen(QColor(255, 255, 255, 170));
  painter->drawText(status_rect.adjusted(12, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                    running ? QString("Playing") : StatusLabel(status));

  // Border last, so selection reads on top of the artwork.
  painter->setBrush(Qt::NoBrush);
  if (running) {
    painter->setPen(QPen(QColor("#43a047"), 2));
    painter->drawPath(path);
  }
  if (selected) {
    painter->setPen(QPen(QColor("#5c6bc0"), 3));
    painter->drawPath(path);
  } else if (hovered) {
    painter->setPen(QPen(QColor(255, 255, 255, 80), 2));
    painter->drawPath(path);
  }
  painter->restore();
}

}  // namespace mira_gui
