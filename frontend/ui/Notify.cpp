#include "Notify.h"

#include "SystemNotifier.h"
#include "Theme.h"

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
// error popup are recognisably the same kind of message.
QString AccentFor(Level level) {
  const theme::Tokens& tokens = theme::Current();
  switch (level) {
    case Level::Success: return tokens.success.name();
    case Level::Warning: return tokens.warning.name();
    case Level::Error: return tokens.error.name();
    case Level::Info: break;
  }
  return tokens.info.name();
}

// Process-wide, from frontend.toml. Zero means "until dismissed".
int g_timeout_seconds = 0;

constexpr int kMargin = 16;
// Fixed, so a one-line toast and a three-line one are the same shape and
// the stack doesn't reflow as messages come and go.
constexpr int kCardWidth = 320;
constexpr int kCardPadding = 12;
constexpr int kCardSpacing = 8;
constexpr int kTextWidth = kCardWidth - 2 * kCardPadding;
constexpr int kFontPixelSize = 12;
// Higher than a timed toast needs, since the default is untimed and
// pushing a card out means discarding something unread. Still bounded.
constexpr int kMaxVisible = 6;
constexpr const char* kHostName = "mira_toast_host";

// Process-wide, set once from frontend.toml at startup — a per-window
// setting would let two windows disagree about desktop notifications.
Delivery g_delivery = Delivery::Auto;

// One card. Click anywhere on it to dismiss.
//
// Not a QObject subclass: overriding mousePressEvent needs no signals, and
// staying out of moc's way avoids a generated-include dance for no gain.
class ToastCard : public QFrame {
public:
  ToastCard(Level level, const QString& text, QWidget* parent) : QFrame(parent) {
    setCursor(Qt::PointingHandCursor);
    const theme::Tokens& tokens = theme::Current();
    setStyleSheet(QString("QFrame { background: %1; border-radius: %2px; "
                          "border-left: 4px solid %3; }"
                          "QLabel { color: %4; }")
                      .arg(tokens.surface_alt.name())
                      .arg(tokens.radius_toast)
                      .arg(AccentFor(level))
                      .arg(tokens.text.name()));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kCardPadding, kCardPadding, kCardPadding, kCardPadding);
    auto* label = new QLabel(text, this);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    // Set here, not in the stylesheet: height is measured from this font
    // below, and a stylesheet is applied too late to be measured.
    QFont font = label->font();
    font.setPixelSize(kFontPixelSize);
    label->setFont(font);
    layout->addWidget(label);

    label_ = label;
    label->setFixedWidth(kTextWidth);
    setFixedWidth(kCardWidth);
  }

  // Settled here, not in the constructor: a word-wrapped QLabel can't say
  // how tall it is until its final font is polished onto it, which happens
  // on the way to being shown. Measuring early clipped the second line.
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
// A child, not a separate top-level: a floating notification window would
// be a second taskbar entry, wouldn't follow its owning window, and on
// Wayland couldn't reliably position itself at all.
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

    // Oldest first, so a burst of events can't push the window off screen.
    while (layout()->count() > kMaxVisible) {
      // dynamic_cast, not qobject_cast: these cards carry no Q_OBJECT, so
      // there's no meta-object for qobject_cast to consult.
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
    // Otherwise it stays until clicked or pushed out. The card's own
    // destruction shrinks the stack, so the host must follow it back down.
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
    // Summed, not taken from layout()->sizeHint(), which kept answering
    // with one card's worth regardless of how many were in it.
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

// Every popup goes through here. PlainText is not optional: an error
// message quoting a path or shell fragment would otherwise be parsed as
// rich text and partly swallowed.
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
  // Cancel keeps focus on anything destructive — a stray Return should
  // never pick the dangerous answer.
  box.setDefaultButton(destructive ? cancel : go);
  box.exec();
  return box.clickedButton() == go;
}

Level LevelFromString(const QString& text) {
  if (text == "success") return Level::Success;
  if (text == "warning") return Level::Warning;
  if (text == "error") return Level::Error;
  return Level::Info;
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
  // isActiveWindow(), not isVisible(): a window can be fully mapped and
  // still be behind others, where a card drawn inside it goes unseen.
  const bool unattended = window == nullptr || !window->isActiveWindow();

  const bool use_system = g_delivery == Delivery::System ||
                          (g_delivery == Delivery::Auto && unattended);
  if (use_system && system_notifier::Send(level, text)) return;

  // Falls through to the card whenever the system route wasn't taken or
  // didn't work, so choosing "system" with no notification service loses
  // nothing.
  if (ToastHost* host = HostFor(parent)) host->Add(level, text);
}

}  // namespace mira_gui::notify
