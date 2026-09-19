#include "Icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QPixmap>

#include <cmath>

#include "Theme.h"

namespace mira_gui::icons {
namespace {

// Everything below is drawn on a 16x16 grid and scaled to whatever size is
// being asked for, so one description covers every DPI.
constexpr qreal kGrid = 16.0;

QPainterPath Hub(qreal center) {
  QPainterPath hub;
  hub.addEllipse(QPointF(center, center), 2.0, 2.0);
  return hub;
}

void PaintGear(QPainter& painter, const QColor& color) {
  constexpr int kTeeth = 8;
  constexpr qreal kCenter = kGrid / 2;

  // A body with teeth laid on top, rather than one polygon alternating
  // between two radii: that construction gives spikes, which at 16px reads as
  // a sunburst rather than a gear.
  QPainterPath gear;
  gear.addEllipse(QPointF(kCenter, kCenter), 4.9, 4.9);
  for (int tooth = 0; tooth < kTeeth; ++tooth) {
    QPainterPath one;
    one.addRoundedRect(QRectF(kCenter - 1.35, kCenter - 7.1, 2.7, 3.4), 0.7, 0.7);
    QTransform rotation;
    rotation.translate(kCenter, kCenter);
    rotation.rotate(tooth * 360.0 / kTeeth);
    rotation.translate(-kCenter, -kCenter);
    gear = gear.united(rotation.map(one));
  }
  gear = gear.subtracted(Hub(kCenter));

  painter.setPen(Qt::NoPen);
  painter.setBrush(color);
  painter.drawPath(gear);
}

void PaintGlyph(QPainter& painter, Glyph glyph, const QColor& color) {
  QPen pen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);

  switch (glyph) {
    case Glyph::Menu:
      for (const qreal y : {4.5, 8.0, 11.5}) painter.drawLine(QPointF(2.5, y), QPointF(13.5, y));
      return;
    case Glyph::Settings:
      PaintGear(painter, color);
      return;
    case Glyph::Minimize:
      painter.drawLine(QPointF(3.5, 8.0), QPointF(12.5, 8.0));
      return;
    case Glyph::Maximize:
      painter.drawRect(QRectF(3.5, 3.5, 9.0, 9.0));
      return;
    case Glyph::Restore:
      // The front window, then the one behind it clipped to an L so the two
      // do not read as a single grid at 16px.
      painter.drawRect(QRectF(3.0, 5.5, 7.5, 7.0));
      painter.drawPolyline(QPolygonF({QPointF(5.5, 5.0), QPointF(5.5, 3.0), QPointF(13.0, 3.0),
                                      QPointF(13.0, 10.0), QPointF(11.0, 10.0)}));
      return;
    case Glyph::Close:
      painter.drawLine(QPointF(4.0, 4.0), QPointF(12.0, 12.0));
      painter.drawLine(QPointF(12.0, 4.0), QPointF(4.0, 12.0));
      return;
  }
}

QPixmap Render(Glyph glyph, int size, const QColor& color) {
  QPixmap pixmap(size, size);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.scale(size / kGrid, size / kGrid);
  PaintGlyph(painter, glyph, color);
  return pixmap;
}

}  // namespace

QIcon For(Glyph glyph) {
  const QColor color = theme::Current().text;
  QIcon icon;
  for (const int size : {16, 20, 24, 32, 48}) icon.addPixmap(Render(glyph, size, color));
  return icon;
}

}  // namespace mira_gui::icons
