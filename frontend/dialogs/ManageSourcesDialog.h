#pragma once

#include <QDialog>
#include <QString>
#include <cstdint>
#include <vector>

#include "../views/SourcePage.h"

class QVBoxLayout;

// Every source with its status and controls: on/off, shown in the sidebar,
// order, import, open or set up, and remove.
class ManageSourcesDialog : public QDialog {
  Q_OBJECT

 public:
  struct Entry {
    mira_gui::SourceInfo source;
    bool ready = false;
    bool enabled = true;
    int games = 0;
    bool in_sidebar = true;
    std::string account;           // signed-in account, if the source says
    std::int64_t imported_at = 0;  // unix seconds; 0 if never
  };

  explicit ManageSourcesDialog(QWidget* parent = nullptr);

  // Entries in sidebar order; rebuilds the rows.
  void SetEntries(const std::vector<Entry>& entries);

 signals:
  void OpenRequested(QString id);
  void SidebarToggled(QString id, bool shown);
  void EnabledToggled(QString id, bool enabled);
  void MoveRequested(QString id, int delta);  // -1 up, +1 down
  void Imported(QString id);                  // an import finished and changed something
  void Removed(QString id);

 private:
  QWidget* BuildRow(const Entry& entry, bool first, bool last);
  void Import(const Entry& entry, QWidget* status_holder);
  void Remove(const Entry& entry);

  QVBoxLayout* rows_ = nullptr;
};
