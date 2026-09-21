#include "GameTileDelegate.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include "GamePresentation.h"
#include "Theme.h"

namespace mira_gui {
namespace {

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
  const theme::Tokens& tokens = theme::Current();
  const bool selected = option.state & QStyle::State_Selected;
  const bool hovered = option.state & QStyle::State_MouseOver;

  // The gap between tiles is this inset, not QListView::spacing: the grid
  // cell stays the size the zoom slider asked for either way. On hover, the
  // tile grows into part of that gap -- never past it, so it can't touch a
  // neighbor -- and what's left of the gap becomes room for a soft shadow
  // (below), grown outward from the un-hovered inset so neither ever
  // crosses into the next cell.
  const int inset = tokens.tile_spacing;
  const int grow = hovered ? qMin(inset, 2) : 0;
  const QRect rect = option.rect.adjusted(inset - grow, inset - grow, -(inset - grow), -(inset - grow));
  if (rect.isEmpty()) return;

  const QString name = index.data(NameRole).toString();
  const std::string status = index.data(StatusRole).toString().toStdString();
  const bool running = index.data(RunningRole).toBool();

  QPainterPath path;
  if (tokens.radius_tile > 0) {
    path.addRoundedRect(rect, tokens.radius_tile, tokens.radius_tile);
  } else {
    path.addRect(rect);
  }

  if (hovered) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    // Concentric, decreasingly transparent fills: each smaller one paints
    // over the middle of the last, leaving only its own outer ring visible
    // -- the usual cheap stand-in for a real blur. Capped to what's left of
    // the gap after grow above claimed its share, so it can't reach the
    // neighbor either.
    const int shadow_budget = qMax(1, inset - grow);
    for (int i = shadow_budget; i >= 1; --i) {
      painter->setBrush(QColor(0, 0, 0, 8 + (shadow_budget - i) * 6));
      const QRect ring = rect.adjusted(-i, -i, i, i);
      if (tokens.radius_tile > 0) {
        painter->drawRoundedRect(ring, tokens.radius_tile + i, tokens.radius_tile + i);
      } else {
        painter->drawRect(ring);
      }
    }
    painter->restore();
  }

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing);
  painter->setClipPath(path);

  const QPixmap cover = index.data(Qt::DecorationRole).value<QPixmap>();
  if (cover.isNull()) {
    painter->fillRect(rect, tokens.tile_placeholder);
  } else {
    painter->drawPixmap(rect, cover);
  }

  // The scrim keeps a light cover from swallowing the title, drawn
  // regardless of the artwork underneath. +1 on the top edge:
  // QRect::bottom() is the last pixel, so without it the scrim fell one
  // pixel short of the tile's actual bottom edge.
  const int scrim_height = qMin(rect.height(), kScrimHeight);
  const QRect scrim(rect.left(), rect.bottom() - scrim_height + 1, rect.width(), scrim_height);
  QLinearGradient gradient(scrim.bottomLeft(), scrim.topLeft());
  gradient.setColorAt(0.0, QColor(0, 0, 0, tokens.scrim_alpha));
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

  // "Ready" says nothing worth a line on every tile — only a state that
  // needs attention (or Playing) earns one.
  if (running || status != "ready") {
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
  }

  // Border last, so selection reads on top of the artwork.
  painter->setBrush(Qt::NoBrush);
  if (running) {
    painter->setPen(QPen(tokens.running, 2));
    painter->drawPath(path);
  }
  if (selected) {
    painter->setPen(QPen(tokens.accent, 3));
    painter->drawPath(path);
  } else if (hovered) {
    painter->setPen(QPen(QColor(255, 255, 255, 80), 2));
    painter->drawPath(path);
  }
  painter->restore();
}

}  // namespace mira_gui
