#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPixmap>
#include <QSize>
#include <QString>

#include <set>
#include <string>
#include <vector>

#include "../client/EventStream.h"
#include "../client/Types.h"
#include "../ui/ArtworkStore.h"
#include "../ui/Shortcuts.h"

class QLabel;
class QLineEdit;
class QComboBox;
class QListWidget;
class QPushButton;
class QSlider;
class QSplitter;
class QStackedWidget;
class QAction;
class QToolButton;

namespace mira_gui {
class GameDetailsPanel;
class GameEditForm;
class GameTileDelegate;
class SettingsPanel;
}

// The primary library view: a cover-art grid, a details panel, and a custom
// top bar (filters, sort, search, tile size, settings, window controls) in
// place of a native titlebar — inspired by Lutris. Frameless, so it owns its
// own move/resize/minimize/maximize/close (see the RootWidget/eventFilter in
// the .cpp); there is no OS decoration to fall back on.
//
// Deliberately a peer of MainWindow rather than a replacement. MainWindow
// (the table view) stays reachable from the View menu and from `mira-gui
// --classic`: it shows every field at once and is still the better tool for
// auditing a freshly scanned library, which is exactly what a cover grid is
// bad at. Both are thin clients over the same MiradClient calls, so neither
// can drift into holding state the other doesn't have.
//
// Selection model, matching Playnite: one click selects a tile and fills the
// details panel, a second (double) click launches, right-click opens the
// per-game menu. Launching on the first click would make a misclick start a
// game, so the details panel is always the first thing a click produces.
class LibraryWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit LibraryWindow(QWidget* parent = nullptr);

private:
  QWidget* BuildTopBar();
  QWidget* BuildGrid();
  QWidget* BuildSettingsPage();
  void BuildMenus();
  void BuildShortcuts();

  // The frontend's own state (size, tile size, which filter) round-trips
  // through frontend.toml, not settings.toml — see FrontendPrefs. Applied
  // after the window is already up, so a slow or absent daemon costs a
  // visible resize rather than a blank window.
  void LoadPrefs();
  void SavePrefs();
  void closeEvent(QCloseEvent* event) override;
  void changeEvent(QEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void ToggleMaximize();

  // `force_scan` separates the two callers: startup, which honours the
  // scan_on_startup preference, and the Refresh command, which does not.
  // Asking for a refresh and getting no scan is the preference answering a
  // question nobody asked it.
  void RefreshHealth(bool force_scan = false);
  void RescanAndRefreshGames(bool force_scan);
  void RefreshGames();

  // games_ is the whole library as last fetched; the grid is a filtered
  // projection of it. Filtering client-side (rather than re-fetching with
  // ?status=) is what lets the search box feel instant and lets "Playing
  // now" and "Never played" be filters at all — neither is a server-side
  // query.
  void ApplyFilter();
  bool MatchesFilter(const mira_gui::GameSummary& game) const;
  QString CurrentFilterKey() const;
  void UpsertGame(const mira_gui::GameSummary& game);
  void RemoveGame(const std::string& id);
  const mira_gui::GameSummary* FindGame(const std::string& id) const;

  QPixmap CoverFor(const mira_gui::GameSummary& game);
  void SetTileWidth(int width);
  QSize TileSize() const;

  void SelectionChanged();
  void SelectGridItem(const std::string& id);
  void ShowContextMenu(const QPoint& pos);
  void ToggleRunning(const std::string& id);
  void ToggleHidden(const std::string& id);
  void LaunchGame(const std::string& id);
  void OpenGameDialog(const std::string& id);
  QWidget* BuildGameEditPage(const std::string& id);
  void CloseGameEdit();
  void OpenSettings();
  void CloseSettings();
  // Confirms first if settings_panel_ is dirty — the top bar's Back button.
  void RequestCloseSettings();
  bool SettingsOpen() const;
  // Gear <-> Back/Save, and greys out the library controls either way.
  void SetSettingsChromeVisible(bool settings_open);
  void OpenRunners();
  void ImportSteamLibrary();
  void OpenClassicView();
  // `announce` is false for the bulk path, where one toast covers the batch
  // and per-game messages would be one notification per game.
  void RefreshMetadata(const std::string& id, bool announce = true);
  void FetchMissingArtwork();
  void ShowSteamGridDbNotice(bool asked_for);
  void UpdateTileCover(const QString& id);

  void HandleGameEvent(const std::string& type, const std::string& data);
  int FilterRow(const QString& key) const;

  QWidget* top_bar_ = nullptr;
  QToolButton* menu_button_ = nullptr;
  QLineEdit* search_ = nullptr;
  QComboBox* filters_ = nullptr;
  QListWidget* grid_ = nullptr;
  mira_gui::GameTileDelegate* delegate_ = nullptr;
  QSlider* zoom_ = nullptr;
  QComboBox* sort_ = nullptr;
  QToolButton* sort_direction_ = nullptr;
  // The gear and the Back/Save pair are siblings, one shown at a time — see
  // SetSettingsChromeVisible.
  QToolButton* settings_button_ = nullptr;
  QWidget* settings_actions_widget_ = nullptr;
  QPushButton* settings_back_button_ = nullptr;
  QPushButton* settings_save_button_ = nullptr;
  QToolButton* minimize_button_ = nullptr;
  QToolButton* maximize_button_ = nullptr;
  QToolButton* close_button_ = nullptr;

  QSplitter* splitter_ = nullptr;
  // Swaps the whole splitter (grid + sidebar) out for settings, full-screen
  // — there's no left sidebar left to keep visible next to it.
  QStackedWidget* content_stack_ = nullptr;
  // Rebuilt on every OpenSettings() so it starts synced to what's actually
  // saved, not stale edits left over from a discarded previous open.
  QWidget* settings_page_ = nullptr;
  mira_gui::SettingsPanel* settings_panel_ = nullptr;
  // The splitter's right slot: page 0 is details_, page 1 is a game's
  // editable form taking over that space (game_settings_in_sidebar pref).
  QStackedWidget* sidebar_stack_ = nullptr;
  QWidget* game_edit_page_ = nullptr;
  mira_gui::GameEditForm* game_edit_form_ = nullptr;
  bool game_settings_in_sidebar_ = true;
  bool restoring_selection_ = false;  // re-entrancy guard for SelectGridItem's own selection change
  QLabel* footer_ = nullptr;
  QLabel* empty_hint_ = nullptr;
  mira_gui::GameDetailsPanel* details_ = nullptr;

  std::vector<mira_gui::GameSummary> games_;
  std::set<std::string> running_ids_;
  // Whether RefreshGames() has ever completed successfully.
  bool loaded_ = false;
  // Games the user explicitly asked to refresh, so that a metadata failure
  // for one of them is worth a toast and the dozens from an automatic scan
  // are not.
  std::set<std::string> awaiting_metadata_;
  bool steamgriddb_notice_shown_ = false;
  std::string selected_id_;
  // The tile width Ctrl+0 returns to, and the one a frontend.toml with
  // no tile_width starts at.
  static constexpr int kDefaultTileWidth = 168;
  int tile_width_ = kDefaultTileWidth;
  std::string sort_key_ = "name";
  bool sort_descending_ = false;
  bool scan_on_startup_ = true;
  std::string notifications_ = "auto";
  // Keyed by "<id>@<tile width>" — a generated cover is cheap but not free,
  // and ApplyFilter() rebuilds every visible tile on each keystroke.
  mira_gui::ArtworkStore* artwork_ = nullptr;
  mira_gui::shortcuts::Common common_;

  mira_gui::EventStream event_stream_;
};
