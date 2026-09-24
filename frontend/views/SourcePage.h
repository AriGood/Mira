#pragma once

#include <QColor>
#include <QPoint>
#include <QHash>
#include <QSet>
#include <QString>
#include <QWidget>

#include <set>
#include <string>
#include <vector>

#include "../client/EventStream.h"
#include "../client/Types.h"

class QLabel;
class QLineEdit;
class QListWidgetItem;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class ArtworkStore;
class DownloadTracker;
class TileGrid;

struct SourceInfo {
  enum class Kind {
    Store,     // a helper tool, an account, and what the account owns
    Launcher,  // a Windows launcher installed into its own prefix
    Local,     // reads what another program already installed
  };
  QString id;    // mirad's own name: "steam", "epic", ...; also the games' `source`
  QString name;  // shown in the sidebar and as the page title
  Kind kind = Kind::Local;
  QColor color;  // the page banner's tint
};

// Every source the sidebar lists, in order.
const std::vector<SourceInfo>& AllSources();

// One store, launcher or other program's page in the library window: the
// games that came from it as cover tiles, whatever setup it still needs, and
// (stores) what the account owns but hasn't installed. Rebuilt on every
// open, so it starts from mirad's current state.
class SourcePage : public QWidget {
  Q_OBJECT

public:
  SourcePage(const SourceInfo& source, ArtworkStore* artwork, DownloadTracker* downloads,
             QWidget* parent = nullptr);

  // The whole library; the page shows the games whose source is this one.
  void SetGames(const std::vector<GameSummary>& games, const std::set<std::string>& running);
  void UpdateCover(const QString& id);
  // Starts an update of an installed store title.
  void UpdateTitle(const QString& ref);

signals:
  void BackRequested();
  // Games were imported or installed; the grid should relist.
  void LibraryChanged();
  void OpenSettingsRequested(QString focus_key);
  void PlayRequested(QString id);
  void OpenGameRequested(QString id);
  // Right-click on an installed game: the library's own game menu, plus
  // Update when `update_ref` is set.
  void GameMenuRequested(QString id, QPoint global_pos, QString update_ref);

private:
  bool IsStore() const { return source_.kind == SourceInfo::Kind::Store; }
  bool IsLauncher() const { return source_.kind == SourceInfo::Kind::Launcher; }
  bool HasImport() const;
  bool HasOwned() const;
  bool IsOwnGame(const GameSummary& game) const;

  QWidget* BuildBanner();
  QWidget* BuildSetupCard();
  QWidget* BuildLibrarySection();
  QWidget* BuildOwnedSection();

  void RefreshStatus();
  void ApplyStoreStatus(const StoreStatusResult& status);
  void ApplyLauncher(const LauncherInfo& launcher);
  void UpdateStatusLine();
  void OpenLogin();
  void SignIn();
  void Import();
  void RefreshOwned();
  void ShowOwned(const StoreLibraryResult& result);
  void ShowBundles(const HumbleLibraryResult& result);
  void RebuildOwnedTiles();
  void StartInstall(const QString& ref, bool update);
  void ApplyFilter();
  void ShowLibraryMenu(const QPoint& pos);
  void HandleEvent(const std::string& type, const std::string& data);

  SourceInfo source_;
  std::string id_;
  ArtworkStore* artwork_ = nullptr;
  DownloadTracker* downloads_ = nullptr;
  bool tool_installed_ = false;
  bool authenticated_ = false;
  bool launcher_installed_ = false;
  bool launcher_installing_ = false;
  std::string account_;
  std::string login_url_;
  int library_count_ = 0;

  QLabel* status_line_ = nullptr;
  QPushButton* banner_primary_ = nullptr;  // Open launcher / Sign out
  QLineEdit* filter_ = nullptr;

  // Setup card: the steps still to do before the rest of the page works.
  QWidget* setup_card_ = nullptr;
  QLabel* setup_title_ = nullptr;
  QLabel* setup_text_ = nullptr;
  QPushButton* setup_button_ = nullptr;  // download the tool / install the launcher
  QWidget* sign_in_row_ = nullptr;
  QPushButton* open_login_ = nullptr;
  QLineEdit* credential_ = nullptr;
  QPushButton* sign_in_ = nullptr;
  QLabel* setup_error_ = nullptr;

  QLabel* library_heading_ = nullptr;
  QPushButton* import_button_ = nullptr;
  QLabel* import_result_ = nullptr;
  TileGrid* library_grid_ = nullptr;
  QLabel* library_empty_ = nullptr;

  QWidget* owned_section_ = nullptr;
  QLabel* owned_heading_ = nullptr;
  QPushButton* owned_refresh_ = nullptr;
  QLabel* owned_note_ = nullptr;
  QPushButton* steam_settings_ = nullptr;
  QPushButton* art_key_ = nullptr;  // covers need a SteamGridDB key
  TileGrid* owned_grid_ = nullptr;

  // What the account owns and isn't installed, by ref, with its title.
  std::vector<std::pair<QString, QString>> owned_;
  QSet<QString> not_owned_;  // refs listed from a collection but not bought
  // Refs asked to install/update/download and not yet started by mirad, and
  // the ones done this session. What's running comes from downloads_.
  QHash<QString, QString> owned_state_;  // ref -> "Installing…", "Downloaded", ...

  EventStream event_stream_;
};

}  // namespace mira_gui
