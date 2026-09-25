#pragma once

#include <QWidget>

#include <string>
#include <vector>

#include "../client/Types.h"

class QButtonGroup;
class QLabel;
class QVBoxLayout;

namespace mira_gui {

class DownloadTracker;

// The library window's Runners page: the Proton or Wine builds installed,
// which one Windows games default to, and the builds available to download.
// Rebuilt on every open, so it starts from mirad's current state.
class RunnersPage : public QWidget {
  Q_OBJECT

public:
  explicit RunnersPage(DownloadTracker* downloads, QWidget* parent = nullptr);

  // The whole library, to say how many games use each build.
  void SetGames(const std::vector<GameSummary>& games);

private:
  std::string CurrentKind() const;
  void Refresh();
  void RefreshInstalled();
  void RefreshCatalog();
  void RebuildInstalled();
  void RebuildCatalog();
  void SetDefault(const std::string& reference);
  void Remove(const RunnerInfo& runner);
  void Download(const std::string& tag);
  void DownloadChanged(const QString& key);
  void SetStatus(const QString& text, bool error = false);

  DownloadTracker* downloads_ = nullptr;
  QButtonGroup* kinds_ = nullptr;
  QVBoxLayout* installed_list_ = nullptr;
  QLabel* default_note_ = nullptr;
  QVBoxLayout* catalog_list_ = nullptr;
  QLabel* status_ = nullptr;

  std::vector<GameSummary> games_;
  std::vector<RunnerInfo> runners_;  // every kind, as last listed
  std::vector<RunnerRelease> releases_;  // CurrentKind()'s
  bool catalog_loaded_ = false;
  std::string default_windows_;  // default_runner.windows
};

}  // namespace mira_gui
