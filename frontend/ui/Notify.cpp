#include "Notify.h"

#include "SystemNotifier.h"

#include <QEvent>

#include <algorithm>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QShowEvent>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace mira_gui::notify {
namespace {

// The accent for each level, shared by both shapes so an error toast and an
// error popup are recognisably the same kind of message. The two that
// already existed elsewhere in the UI (the Online/Offline badge) are reused
// rather than re-picked.
QString AccentFor(Level level) {
  switch (level) {
    case Level::Success: return "#2e7d32";
    case Level::Warning: return "#ef6c00";
    case Level::Error: return "#c62828";
    case Level::Info: break;
  }
  return "#1e88e5";
}

// Process-wide, from frontend.toml. Zero means "until dismissed" — see
// Notify.h for why that is the default rather than a curiosity.
int g_timeout_seconds = 0;

constexpr int kMargin = 16;
// Fixed, so that a one-line toast and a three-line one are the same shape
// and the stack does not reflow as messages come and go.
constexpr int kCardWidth = 320;
constexpr int kCardPadding = 12;
constexpr int kCardSpacing = 8;
constexpr int kTextWidth = kCardWidth - 2 * kCardPadding;
constexpr int kFontPixelSize = 12;
// Higher than it needs to be for a timed toast, because the default is
// untimed: with "until dismissed", pushing a card out of the stack is
// discarding something nobody has read. Still bounded — a window has a
// bottom edge, and a burst has to stop somewhere.
constexpr int kMaxVisible = 6;
constexpr const char* kHostName = "mira_toast_host";

// Process-wide, set once from frontend.toml at startup. A per-window
// setting would let two windows of the same application disagree about
// whether the desktop should be told things.
Delivery g_delivery = Delivery::Auto;

// One card. Click anywhere on it to dismiss.
//
// Deliberately not a QObject subclass with signals: overriding
// mousePressEvent is a virtual call, and staying out of moc's way keeps
// this file free of a generated-include dance for no gain.
class ToastCard : public QFrame {
public:
  ToastCard(Level level, const QString& text, QWidget* parent) : QFrame(parent) {
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QString("QFrame { background: #2b2b2b; border-radius: 6px; "
                          "border-left: 4px solid %1; }"
                          "QLabel { color: #f0f0f0; }")
                      .arg(AccentFor(level)));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kCardPadding, kCardPadding, kCardPadding, kCardPadding);
    auto* label = new QLabel(text, this);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    // Set here rather than in the stylesheet above, because the height is
    // measured from this font a few lines down and a stylesheet is applied
    // too late to be measured.
    QFont font = label->font();
    font.setPixelSize(kFontPixelSize);
    label->setFont(font);
    layout->addWidget(label);

    label_ = label;
    label->setFixedWidth(kTextWidth);
    setFixedWidth(kCardWidth);
  }

  // The height is settled here rather than in the constructor. A
  // word-wrapped QLabel cannot say how tall it is until its width and its
  // final font are both decided, and the font is not final until the
  // stylesheet above has been polished onto it — which happens on the way
  // to being shown, not on the way out of the constructor. Measuring early
  // produced a one-line card under two lines of text, with the second line
  // clipped.
  void showEvent(QShowEvent* event) override {
    QFrame::showEvent(event);
    const int measured = label_->heightForWidth(kTextWidth);
    const int text_height = measured > 0 ? measured : label_->sizeHint().height();
    label_->setFixedHeight(text_height);
    setFixedHeight(text_height + 2 * kCardPadding);
  }

  void Dismiss() {
    if (dismissing_) return;
    dismissing_ = true;

    auto* fade = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(fade);
    auto* animation = new QPropertyAnimation(fade, "opacity", this);
    animation->setDuration(220);
    animation->setStartValue(1.0);
    animation->setEndValue(0.0);
    connect(animation, &QPropertyAnimation::finished, this, &QObject::deleteLater);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
  }

protected:
  void mousePressEvent(QMouseEvent* event) override {
    Dismiss();
    event->accept();
  }

private:
  QLabel* label_ = nullptr;
  bool dismissing_ = false;
};

// The stack the cards live in: a plain child widget pinned to the bottom
// right of the window, resized to its contents.
//
// A child rather than a separate top-level: a floating notification window
// would be a second entry in the taskbar, would not follow the window it
// belongs to, and on Wayland could not reliably position itself at all.
class ToastHost : public QWidget {
public:
  explicit ToastHost(QWidget* window) : QWidget(window) {
    setObjectName(kHostName);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kCardSpacing);
    setFixedWidth(kCardWidth);
    window->installEventFilter(this);
  }

  void Add(Level level, const QString& text) {
    auto* card = new ToastCard(level, text, this);
    layout()->addWidget(card);

    // Oldest first, so a burst of events (a scan finishing, then five
    // metadata fetches) cannot push the window off its own screen.
    while (layout()->count() > kMaxVisible) {
      // dynamic_cast, not qobject_cast: these cards deliberately carry no
      // Q_OBJECT, so there is no meta-object for qobject_cast to consult.
      if (auto* oldest = dynamic_cast<ToastCard*>(layout()->itemAt(0)->widget())) {
        oldest->Dismiss();
        layout()->removeWidget(oldest);
      } else {
        break;
      }
    }

    // Shown before Reposition, so its showEvent has settled its height by
    // the time the stack is measured.
    card->show();

    if (const int seconds = CurrentTimeoutSeconds(); seconds > 0) {
      QTimer::singleShot(seconds * 1000, card, [card] { card->Dismiss(); });
    }
    // Otherwise it stays until clicked, or until kMaxVisible pushes it out.
    // The card's own destruction shrinks the stack, so the host has to
    // follow it back down as well as up.
    connect(card, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, [this] {
                                                       Reposition();
                                                     }); });
    show();
    raise();
    Reposition();
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == parentWidget() && event->type() == QEvent::Resize) Reposition();
    return QWidget::eventFilter(watched, event);
  }

private:
  void Reposition() {
    QWidget* window = parentWidget();
    if (window == nullptr) return;
    // Summed rather than taken from layout()->sizeHint(), which kept
    // answering with one card's worth however many were in it. The cards
    // have fixed heights, so there is nothing here a layout could work out
    // that this cannot.
    int stack = 0;
    for (int i = 0; i < layout()->count(); ++i) {
      if (QWidget* card = layout()->itemAt(i)->widget()) stack += card->height() + kCardSpacing;
    }
    resize(kCardWidth, qMax(0, stack - kCardSpacing));
    move(window->width() - width() - kMargin, window->height() - height() - kMargin);
    raise();
  }
};

ToastHost* HostFor(QWidget* parent) {
  if (parent == nullptr) return nullptr;
  QWidget* window = parent->window();
  if (window == nullptr) return nullptr;
  if (QWidget* existing = window->findChild<QWidget*>(kHostName, Qt::FindDirectChildrenOnly)) {
    return static_cast<ToastHost*>(existing);
  }
  return new ToastHost(window);
}

// Every popup goes through here, so that "what failed" and "what mirad
// said" are always in the same two places. PlainText is not optional: an
// error message quoting a path or a shell fragment would otherwise be
// parsed as rich text and partly swallowed.
void ShowMessage(QWidget* parent, QMessageBox::Icon icon, const QString& title,
                 const QString& text, const QString& detail) {
  QMessageBox box(parent);
  box.setIcon(icon);
  box.setWindowTitle(title);
  box.setTextFormat(Qt::PlainText);
  box.setText(text);
  if (!detail.isEmpty()) box.setInformativeText(detail);
  box.setStandardButtons(QMessageBox::Ok);
  box.exec();
}

}  // namespace

void Failed(QWidget* parent, const QString& what, const QString& detail) {
  ShowMessage(parent, QMessageBox::Warning, "Mira", what, detail);
}

void FailedWithHint(QWidget* parent, const QString& what, const QString& detail,
                    const QString& hint) {
  ShowMessage(parent, QMessageBox::Warning, "Mira", what,
              detail.isEmpty() ? hint : detail + "\n\n" + hint);
}

void Info(QWidget* parent, const QString& title, const QString& message) {
  ShowMessage(parent, QMessageBox::Information, title, message, QString());
}

bool Confirm(QWidget* parent, const QString& title, const QString& question, const QString& accept,
             bool destructive) {
  QMessageBox box(parent);
  box.setIcon(destructive ? QMessageBox::Warning : QMessageBox::Question);
  box.setWindowTitle(title);
  box.setTextFormat(Qt::PlainText);
  box.setText(question);

  QPushButton* go = box.addButton(accept, QMessageBox::AcceptRole);
  QPushButton* cancel = box.addButton("Cancel", QMessageBox::RejectRole);
  // Cancel keeps the focus on anything destructive: the dangerous answer
  // should never be the one a stray Return picks.
  box.setDefaultButton(destructive ? cancel : go);
  box.exec();
  return box.clickedButton() == go;
}

void SetDelivery(Delivery delivery) { g_delivery = delivery; }

void SetTimeoutSeconds(int seconds) {
  g_timeout_seconds = std::clamp(seconds, 0, kMaxTimeoutSeconds);
}

int CurrentTimeoutSeconds() { return g_timeout_seconds; }

Delivery CurrentDelivery() { return g_delivery; }

QString DeliveryToString(Delivery delivery) {
  switch (delivery) {
    case Delivery::System: return "system";
    case Delivery::InApp: return "in_app";
    case Delivery::Auto: break;
  }
  return "auto";
}

Delivery DeliveryFromString(const QString& text) {
  if (text == "system") return Delivery::System;
  if (text == "in_app") return Delivery::InApp;
  return Delivery::Auto;
}

void Toast(QWidget* parent, Level level, const QString& text) {
  const QWidget* window = parent != nullptr ? parent->window() : nullptr;
  // isActiveWindow() rather than isVisible(): a window can be fully mapped
  // and still be behind three others, or on another virtual desktop, and in
  // both cases a card drawn inside it is a message nobody receives.
  const bool unattended = window == nullptr || !window->isActiveWindow();

  const bool use_system = g_delivery == Delivery::System ||
                          (g_delivery == Delivery::Auto && unattended);
  if (use_system && system_notifier::Send(level, text)) return;

  // Falls through to the card whenever the system route was not taken or
  // did not work, so choosing "system" on a desktop with no notification
  // service loses nothing.
  if (ToastHost* host = HostFor(parent)) host->Add(level, text);
}

}  // namespace mira_gui::notify
