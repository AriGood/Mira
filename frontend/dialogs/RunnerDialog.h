#pragma once

#include <QDialog>

#include <set>
#include <string>

#include "../client/EventStream.h"
#include "../client/Types.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

// Runner management: what's installed and what's available to install,
// with a Download button for the latter.
//
// Holds its own EventStream because a download finishes on the daemon's
// schedule, not the request's — the download call returns 202 immediately,
// and the outcome arrives later as `runners.download.finished`/`.failed`.
// The list refreshes on that event, not on the reply.
class RunnerDialog : public QDialog {
  Q_OBJECT

public:
  explicit RunnerDialog(QWidget* parent = nullptr);

private:
  void RefreshInstalled();
  void RefreshCatalog();
  void DownloadSelected();
  void RemoveSelected();
  void ShowSchema();
  void HandleEvent(const std::string& type, const std::string& data);
  void SetStatus(const QString& text, bool error = false);
  std::string CurrentKind() const;

  QComboBox* kind_ = nullptr;
  QTreeWidget* installed_ = nullptr;
  QTreeWidget* catalog_ = nullptr;
  QPushButton* download_ = nullptr;
  QPushButton* remove_ = nullptr;
  QPushButton* schema_ = nullptr;
  QPushButton* refresh_ = nullptr;
  QLabel* status_ = nullptr;

  // Tags currently downloading, so a second click can't queue the same
  // build twice while the first is still running.
  std::set<std::string> downloading_;

  mira_gui::EventStream event_stream_;
};
