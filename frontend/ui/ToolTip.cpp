#include "ToolTip.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QVBoxLayout>

#include <algorithm>

#include "HoverCard.h"

namespace mira_gui::tooltip {
namespace {

// Wider than this wraps.
constexpr int kMaxWidth = 320;

class TipCard : public QWidget {
public:
  TipCard() : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(card::kPaddingX, card::kPaddingY - 3, card::kPaddingX, card::kPaddingY - 3);
    label_ = new QLabel(this);
    label_->setWordWrap(true);
    layout->addWidget(label_);
  }

  void ShowText(const QString& text, const QRect& anchor, bool beside) {
    label_->setText(text);
    label_->ensurePolished();
    const QFontMetrics metrics(label_->font());
    int natural = 0;
    for (const QString& line : text.split('\n')) natural = std::max(natural, metrics.horizontalAdvance(line));
    const int width = std::min(natural + 2, kMaxWidth);
    label_->setFixedSize(width, label_->heightForWidth(width));
    adjustSize();
    move(card::Place(anchor, size(), beside));
    show();
  }

protected:
  void paintEvent(QPaintEvent*) override { card::Paint(this); }

private:
  QLabel* label_ = nullptr;
};

// QAction::toolTip() falls back to the action's text; only a tooltip that
// says something more is worth showing.
QString ExplicitToolTip(const QAction* action) {
  auto stripped = [](QString text) {
    text.remove('&');
    text.remove("...");
    text.remove(QChar(0x2026));
    return text.trimmed();
  };
  const QString tip = action->toolTip();
  return stripped(tip) == stripped(action->text()) ? QString() : tip;
}

QRect GlobalRect(const QWidget* widget, const QRect& local) {
  return QRect(widget->mapToGlobal(local.topLeft()), local.size());
}

class ToolTipFilter : public QObject {
public:
  explicit ToolTipFilter(QObject* parent) : QObject(parent) {
    // Deleted before QApplication is, like any other top-level widget.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
      delete card_;
      card_ = nullptr;
    });
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    switch (event->type()) {
      case QEvent::ToolTip:
        return ShowFor(watched, static_cast<QHelpEvent*>(event));
      case QEvent::Leave:
      case QEvent::Hide:
        if (watched == target_) Hide();
        break;
      case QEvent::MouseMove:
        if (card_ != nullptr && card_->isVisible() && !anchor_.contains(QCursor::pos())) Hide();
        break;
      case QEvent::MouseButtonPress:
      case QEvent::Wheel:
      case QEvent::KeyPress:
      case QEvent::WindowDeactivate:
        Hide();
        break;
      default:
        break;
    }
    return false;
  }

private:
  bool ShowFor(QObject* watched, QHelpEvent* help) {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr) return false;

    QString text;
    QRect anchor;
    bool beside = false;
    if (auto* menu = qobject_cast<QMenu*>(widget)) {
      QAction* action = menu->actionAt(help->pos());
      if (action != nullptr) {
        text = ExplicitToolTip(action);
        // The whole row's height, beside the menu rather than over the next item.
        const QRect row = menu->actionGeometry(action);
        anchor = GlobalRect(menu, QRect(0, row.top(), menu->width(), row.height()));
        beside = true;
      }
    } else if (auto* view = qobject_cast<QAbstractItemView*>(widget->parentWidget());
               view != nullptr && view->viewport() == widget) {
      const QModelIndex index = view->indexAt(help->pos());
      text = index.data(Qt::ToolTipRole).toString();
      anchor = GlobalRect(widget, view->visualRect(index));
    } else {
      text = widget->toolTip();
      // Qt passes it on to the parent, which may have one.
      if (text.isEmpty()) return false;
      anchor = GlobalRect(widget, widget->rect());
    }

    if (text.isEmpty()) {
      Hide();
      return true;
    }
    if (card_ == nullptr) card_ = new TipCard();
    target_ = widget;
    anchor_ = anchor;
    card_->ShowText(text, anchor, beside);
    return true;
  }

  void Hide() {
    if (card_ != nullptr) card_->hide();
    target_ = nullptr;
  }

  TipCard* card_ = nullptr;
  QPointer<QWidget> target_;
  QRect anchor_;
};

}  // namespace

void Install() { qApp->installEventFilter(new ToolTipFilter(qApp)); }

}  // namespace mira_gui::tooltip
