#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {
struct ItchCollectionsResult;
}

// The itch.io collections whose games show on the itch page: the account's
// own, plus any added by link. Added ones can be removed again.
class ItchCollectionsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit ItchCollectionsDialog(QWidget* parent = nullptr);

  // True once a collection was added or removed, so the page relists.
  bool Changed() const { return changed_; }

 private:
  void Refresh();
  void ShowCollections(const mira_gui::ItchCollectionsResult& result);
  void Add();

  QVBoxLayout* list_ = nullptr;
  QLabel* status_ = nullptr;
  QLineEdit* link_ = nullptr;
  QPushButton* add_ = nullptr;
  bool changed_ = false;
};
