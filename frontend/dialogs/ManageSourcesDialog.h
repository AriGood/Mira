#pragma once

#include <QDialog>
#include <QString>
#include <vector>

#include "../views/SourcePage.h"

// Every source with its status: set up ones get an "In sidebar" toggle and
// Open, the rest a Set up button that opens their page.
class ManageSourcesDialog : public QDialog {
  Q_OBJECT

 public:
  struct Entry {
    mira_gui::SourceInfo source;
    bool ready = false;
    int games = 0;
    bool in_sidebar = true;
  };

  explicit ManageSourcesDialog(const std::vector<Entry>& entries, QWidget* parent = nullptr);

 signals:
  void OpenRequested(QString id);
  void SidebarToggled(QString id, bool shown);

 private:
  QWidget* BuildRow(const Entry& entry);
};
