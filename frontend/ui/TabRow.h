#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

class QHBoxLayout;
class QPushButton;

namespace mira_gui {

// A page's tabs, each with a count, then the page's own actions at the end.
// At most one tab is current; none is when the page shows something no tab
// names (a filter picked from elsewhere).
class TabRow : public QWidget {
  Q_OBJECT

public:
  explicit TabRow(QWidget* parent = nullptr);

  void AddTab(const QString& key, const QString& label);
  void SetCount(const QString& key, int count);
  // Draws the count in the warning color, for a tab that wants a look.
  void SetAlert(const QString& key, bool alert);
  void SetCurrent(const QString& key);
  QString Current() const;
  void SetTabsVisible(bool visible);
  // Appends `widget` after the tabs, right-aligned.
  void SetTrailing(QWidget* widget);

signals:
  void CurrentChanged(QString key);

private:
  struct Tab {
    QPushButton* button = nullptr;
    QString label;
    int count = -1;
    bool alert = false;
  };
  void Relabel(Tab& tab);

  QHBoxLayout* tabs_ = nullptr;
  QHBoxLayout* layout_ = nullptr;
  QHash<QString, Tab> by_key_;
  QString current_;
};

// "● " in `color`, for a status line.
QString StatusDot(const QColor& color);

}  // namespace mira_gui
