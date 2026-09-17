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

// Runner management: what's installed (`GET /v1/runners`) and what's
// available to install (`GET /v1/runners/catalog`), with a Download button
// for the latter.
//
// Holds its own EventStream because a download is the one thing in the
// frontend that finishes on the daemon's schedule rather than the
// request's: `POST /v1/runners/download` returns 202 as soon as it starts,
// and the outcome arrives later as `runners.download.finished`/`.failed`
// (a build can be 500+ MB and there is no job queue yet — docs/api.md). A
// dialog that closed before then would simply miss it, which is why the
// list refreshes itself on the event rather than on the reply.
class RunnerDialog : public QDialog {
  Q_OBJECT

public:
  explicit RunnerDialog(QWidget* parent = nullptr);

private:
  void RefreshInstalled();
  void RefreshCatalog();
  void DownloadSelected();
  void HandleEvent(const std::string& type, const std::string& data);
  void SetStatus(const QString& text, bool error = false);
  std::string CurrentKind() const;

  QComboBox* kind_ = nullptr;
  QTreeWidget* installed_ = nullptr;
  QTreeWidget* catalog_ = nullptr;
  QPushButton* download_ = nullptr;
  QPushButton* refresh_ = nullptr;
  QLabel* status_ = nullptr;

  // Tags currently downloading, so a second click can't queue the same
  // build twice while the first is still running.
  std::set<std::string> downloading_;

  mira_gui::EventStream event_stream_;
};
