#include "SettingsNav.h"

#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace mira_gui {

SettingsNavWidget::SettingsNavWidget(QWidget* parent) : QWidget(parent) {
  auto* outer = new QHBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* left = new QWidget(this);
  // Wide enough that SettingsPanel's "Show advanced & expert settings"
  // header checkbox (the longest thing this column ever holds) doesn't clip.
  left->setFixedWidth(240);
  left_layout_ = new QVBoxLayout(left);
  left_layout_->setContentsMargins(0, 0, 12, 0);
  left_layout_->setSpacing(8);

  search_ = new QLineEdit(left);
  search_->setObjectName("settings_search");
  search_->setClearButtonEnabled(true);
  search_->setPlaceholderText("Search settings…");
  connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { ApplyFilter(); });
  left_layout_->addWidget(search_);

  nav_list_ = new QListWidget(left);
  nav_list_->setObjectName("settings_nav");
  connect(nav_list_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row >= 0) stack_->setCurrentIndex(row);
  });
  left_layout_->addWidget(nav_list_, /*stretch=*/1);

  outer->addWidget(left);

  stack_ = new QStackedWidget(this);
  stack_->setObjectName("settings_content");

  empty_state_ = new QLabel("No settings match your search.", this);
  empty_state_->setAlignment(Qt::AlignCenter);
  empty_state_->setProperty("role", "muted");

  content_stack_ = new QStackedWidget(this);
  content_stack_->addWidget(stack_);
  content_stack_->addWidget(empty_state_);
  outer->addWidget(content_stack_, /*stretch=*/1);
}

QFormLayout* SettingsNavWidget::AddCategory(const QString& title) {
  auto* page = new QWidget(this);
  auto* form = new QFormLayout(page);
  form->setVerticalSpacing(10);
  form->setHorizontalSpacing(14);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);

  auto* scroll = new QScrollArea(this);
  scroll->setWidget(page);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  stack_->addWidget(scroll);

  Category category;
  category.page = page;
  category.scroll = scroll;
  category.form = form;
  category.item = new QListWidgetItem(title, nav_list_);
  categories_.push_back(category);
  category_by_form_.insert(form, static_cast<int>(categories_.size()) - 1);

  if (nav_list_->currentRow() < 0) nav_list_->setCurrentRow(0);
  return form;
}

void SettingsNavWidget::RegisterRow(QFormLayout* form, QWidget* row_widget,
                                    const QString& searchable_text) {
  const auto category_it = category_by_form_.constFind(form);
  if (category_it == category_by_form_.constEnd()) return;  // form wasn't returned by AddCategory

  RowEntry row;
  row.form = form;
  row.row_widget = row_widget;
  row.search_text = searchable_text.toLower();
  row.category_index = category_it.value();
  rows_.push_back(row);
  row_index_by_widget_.insert(row_widget, static_cast<int>(rows_.size()) - 1);
  categories_[row.category_index].total_rows++;
}

void SettingsNavWidget::SetRowGateVisible(QWidget* row_widget, bool visible) {
  const auto it = row_index_by_widget_.constFind(row_widget);
  if (it == row_index_by_widget_.constEnd()) return;
  rows_[it.value()].gate_visible = visible;
  ApplyFilter();
}

void SettingsNavWidget::ApplyFilter() {
  const QString query = search_->text().trimmed().toLower();
  std::vector<int> visible_count(categories_.size(), 0);

  for (const RowEntry& row : rows_) {
    const bool visible = row.gate_visible && (query.isEmpty() || row.search_text.contains(query));
    row.form->setRowVisible(row.row_widget, visible);
    if (visible) visible_count[row.category_index]++;
  }

  bool any_visible = false;
  for (size_t i = 0; i < categories_.size(); ++i) {
    const Category& category = categories_[i];
    const bool show = category.total_rows == 0 || visible_count[i] > 0;
    category.item->setHidden(!show);
    any_visible = any_visible || show;
  }

  if (!query.isEmpty() && !any_visible) {
    content_stack_->setCurrentWidget(empty_state_);
    return;
  }
  content_stack_->setCurrentWidget(stack_);

  const int current = nav_list_->currentRow();
  if (current < 0 || nav_list_->item(current)->isHidden()) SelectFirstVisibleCategory();
}

void SettingsNavWidget::SelectFirstVisibleCategory() {
  for (int i = 0; i < nav_list_->count(); ++i) {
    if (!nav_list_->item(i)->isHidden()) {
      nav_list_->setCurrentRow(i);
      return;
    }
  }
}

void SettingsNavWidget::RevealRow(QWidget* row_widget) {
  const auto it = row_index_by_widget_.constFind(row_widget);
  if (it == row_index_by_widget_.constEnd()) return;
  const RowEntry& row = rows_[it.value()];

  if (!search_->text().isEmpty()) search_->clear();  // triggers ApplyFilter via textChanged

  const Category& category = categories_[row.category_index];
  nav_list_->setCurrentItem(category.item);
  category.scroll->ensureWidgetVisible(row_widget);
}

void SettingsNavWidget::SetHeaderWidget(QWidget* widget) {
  if (header_widget_ != nullptr) {
    left_layout_->removeWidget(header_widget_);
    header_widget_->deleteLater();
  }
  header_widget_ = widget;
  left_layout_->insertWidget(0, widget);
}

}  // namespace mira_gui
