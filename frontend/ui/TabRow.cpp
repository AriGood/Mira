#include "TabRow.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QStyle>


namespace mira_gui {

TabRow::TabRow(QWidget* parent) : QWidget(parent) {
  setObjectName("tab_row");
  setAttribute(Qt::WA_StyledBackground, true);
  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(4);
  tabs_ = new QHBoxLayout();
  tabs_->setSpacing(4);
  layout_->addLayout(tabs_);
  layout_->addStretch(1);
}

void TabRow::AddTab(const QString& key, const QString& label) {
  auto* button = new QPushButton(this);
  button->setCheckable(true);
  button->setFlat(true);
  button->setCursor(Qt::PointingHandCursor);
  // Clicking the current tab again keeps it current.
  connect(button, &QPushButton::clicked, this, [this, key] {
    const bool changed = key != current_;
    SetCurrent(key);
    if (changed) emit CurrentChanged(key);
  });
  tabs_->addWidget(button);
  by_key_.insert(key, Tab{button, label});
  Relabel(by_key_[key]);
}

void TabRow::Relabel(Tab& tab) {
  tab.button->setText(tab.count < 0 ? tab.label : QString("%1  %2").arg(tab.label).arg(tab.count));
  tab.button->setProperty("alert", tab.alert && tab.count > 0);
  tab.button->style()->unpolish(tab.button);
  tab.button->style()->polish(tab.button);
}

void TabRow::SetCount(const QString& key, int count) {
  const auto it = by_key_.find(key);
  if (it == by_key_.end() || it->count == count) return;
  it->count = count;
  Relabel(*it);
}

void TabRow::SetAlert(const QString& key, bool alert) {
  const auto it = by_key_.find(key);
  if (it == by_key_.end()) return;
  it->alert = alert;
  Relabel(*it);
}

void TabRow::SetCurrent(const QString& key) {
  current_ = by_key_.contains(key) ? key : QString();
  for (auto it = by_key_.begin(); it != by_key_.end(); ++it) it->button->setChecked(it.key() == current_);
}

QString TabRow::Current() const { return current_; }

void TabRow::SetTabsVisible(bool visible) {
  for (const Tab& tab : by_key_) tab.button->setVisible(visible);
}

void TabRow::SetTrailing(QWidget* widget) { layout_->addWidget(widget); }

QString StatusDot(const QColor& color) {
  return QString("<span style='color:%1'>●</span> ").arg(color.name());
}

}  // namespace mira_gui
