#include "HelpButton.h"

#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QUrl>
#include <QVBoxLayout>

#include "HoverCard.h"

namespace mira_gui {
namespace {

constexpr int kCardWidth = 340;

class HelpCard : public QWidget {
public:
  HelpCard(const QString& text, const QString& link, QWidget* parent)
      : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(card::kPaddingX, card::kPaddingY, card::kPaddingX, card::kPaddingY);
    layout->setSpacing(8);

    auto* body = new QLabel(text, this);
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->setFixedWidth(kCardWidth - 2 * card::kPaddingX);
    layout->addWidget(body);
    if (!link.isEmpty()) layout->addWidget(MakeExternalLink(link, this));
    adjustSize();
  }

protected:
  void paintEvent(QPaintEvent*) override { card::Paint(this); }
};

}  // namespace

HelpButton::HelpButton(const QString& text, const QString& link, QWidget* parent)
    : QToolButton(parent), text_(text), link_(link) {
  setObjectName("help_button");
  setText("?");
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::TabFocus);
  setAccessibleName("Help");
  setFixedSize(18, 18);
  connect(this, &QToolButton::clicked, this, &HelpButton::ShowCard);
}

void HelpButton::ShowCard() {
  auto* help_card = new HelpCard(text_, link_, this);
  const QRect anchor(mapToGlobal(QPoint(0, 0)), size());
  help_card->move(card::Place(anchor, help_card->size(), /*beside=*/false));
  // The popup grabs the mouse, so the button gets no Leave while it is open, and closing the popup
  // hands focus back to the button. Both leave the accent color stuck.
  const bool by_mouse = underMouse();
  connect(help_card, &QObject::destroyed, this, [this, by_mouse] {
    if (by_mouse) clearFocus();
    if (rect().contains(mapFromGlobal(QCursor::pos()))) return;
    setAttribute(Qt::WA_UnderMouse, false);
    update();
  });
  help_card->show();
}

QWidget* LabelWithHelp(const QString& label, const QString& doc, QWidget* parent, const QString& link) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(6);
  layout->addWidget(new QLabel(label, row));
  if (!doc.isEmpty()) layout->addWidget(new HelpButton(doc, link, row));
  layout->addStretch(1);
  return row;
}

QLabel* MakeExternalLink(const QString& url, QWidget* parent) {
  QString host = QUrl(url).host();
  if (host.startsWith("www.")) host.remove(0, 4);
  auto* label = new QLabel(QString("<a href=\"%1\">%2 ↗</a>").arg(url.toHtmlEscaped(), host.toHtmlEscaped()),
                           parent);
  label->setTextFormat(Qt::RichText);
  label->setTextInteractionFlags(Qt::TextBrowserInteraction);
  label->setOpenExternalLinks(true);
  label->setToolTip(url);
  return label;
}

}  // namespace mira_gui
