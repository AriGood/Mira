#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include <string>
#include <vector>

class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace mira_gui {

// Row indices grouped by category, in first-seen order. Never sorted: the
// schema's order is the display order.
struct CategoryRows {
  QString name;
  std::vector<size_t> rows;
};
std::vector<CategoryRows> GroupByCategory(const std::vector<std::string>& categories);

// Left-nav-plus-search chrome shared by the Settings screen and the
// per-game Advanced editor: category list + search on the left, a
// QStackedWidget of per-category forms on the right. Knows nothing about
// config schemas — each consumer's own gate (the overridable filter)
// composes with the live search via SetRowGateVisible instead of
// both fighting over the same row's setRowVisible call.
class SettingsNavWidget : public QWidget {
  Q_OBJECT

public:
  explicit SettingsNavWidget(QWidget* parent = nullptr);

  // Adds a category page (a scroll-wrapped QFormLayout, same shape every
  // category got from the old per-consumer AddCategoryTab) and a matching
  // nav-list entry. Returns the form, ready for addRow calls.
  QFormLayout* AddCategory(const QString& title);

  // Call once per row, right after form->addRow(label, row_widget) — lets
  // the search box find it later. searchable_text should already contain
  // everything the row should match on (key, doc, category, hand-picked
  // synonyms for non-schema rows); matching is a plain case-insensitive
  // substring test, no tokenizing.
  void RegisterRow(QFormLayout* form, QWidget* row_widget, const QString& searchable_text);

  // A second, independent visibility gate under the search filter, e.g. the
  // overridable flag on the per-game editor. Defaults to true. Composes
  // with the live search query rather than racing it: both flow through
  // the same ApplyFilter() pass.
  void SetRowGateVisible(QWidget* row_widget, bool visible);

  // Clears any active search (so the target row can't be hidden by a stale
  // query), switches to row_widget's category, and scrolls it into view.
  // Does not change focus — the caller still owns whichever inner control
  // widget should actually receive it.
  void RevealRow(QWidget* row_widget);

  // A widget shown above the search box, e.g. the main Settings screen's
  // "Show advanced & expert settings" checkbox. Screen-wide chrome that
  // isn't part of any one category, so it lives with the other
  // screen-wide chrome (nav + search) rather than floating separately.
  void SetHeaderWidget(QWidget* widget);

  // A widget pinned below the nav list, e.g. the Settings screen's
  // Back/Reset/Save row. Each call inserts right below the nav list, above
  // whatever an earlier call added.
  void AddFooterWidget(QWidget* widget);

  // A thin line across `form`, between two groups of rows.
  void AddDivider(QFormLayout* form);

private:
  struct Category {
    QListWidgetItem* item = nullptr;
    QWidget* page = nullptr;
    QScrollArea* scroll = nullptr;
    QFormLayout* form = nullptr;
    int total_rows = 0;
  };

  struct RowEntry {
    QFormLayout* form = nullptr;
    QWidget* row_widget = nullptr;
    QString search_text;  // pre-lowercased
    bool gate_visible = true;
    int category_index = -1;
  };

  void ApplyFilter();
  void SelectFirstVisibleCategory();

  QLineEdit* search_ = nullptr;
  QListWidget* nav_list_ = nullptr;
  QStackedWidget* stack_ = nullptr;         // one page per category, indices matching nav_list_ rows
  QStackedWidget* content_stack_ = nullptr;  // stack_ vs. empty_state_
  QLabel* empty_state_ = nullptr;            // shown when a query matches nothing at all
  QVBoxLayout* left_layout_ = nullptr;  // header widget (if any), search box, nav list, footer, in that order
  QWidget* header_widget_ = nullptr;

  std::vector<Category> categories_;
  std::vector<RowEntry> rows_;
  QHash<QWidget*, int> row_index_by_widget_;   // row_widget -> index into rows_
  QHash<QFormLayout*, int> category_by_form_;  // form -> index into categories_
};

}  // namespace mira_gui
