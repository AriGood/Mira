#pragma once

#include <QString>
#include <QToolButton>

class QLabel;

namespace mira_gui {

// A small [?] that opens a setting's explanation in a card on click, instead
// of a hover tooltip. The card stays until a click elsewhere or Escape, so its
// link can be followed.
class HelpButton : public QToolButton {
  Q_OBJECT

public:
  // `link`, when set, is shown under the text as a clickable web link.
  HelpButton(const QString& text, const QString& link, QWidget* parent = nullptr);

private:
  void ShowCard();

  QString text_;
  QString link_;
};

// A form row label with a [?] after it. No button when `doc` is empty.
QWidget* LabelWithHelp(const QString& label, const QString& doc, QWidget* parent,
                       const QString& link = QString());

// "steamgriddb.com ↗" as a clickable link to `url`, for a row that needs a value
// from that site.
QLabel* MakeExternalLink(const QString& url, QWidget* parent);

}  // namespace mira_gui
