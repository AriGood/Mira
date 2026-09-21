#pragma once

#include <QHash>
#include <QList>
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
class QVBoxLayout;
class QAction;
class QToolButton;
class QListWidgetItem;

// QListWidget with setViewportMargins made public — Qt keeps it protected on
// QAbstractScrollArea. Defined in LibraryWindow.cpp; this file only ever
// holds a pointer to it.
class LibraryGrid;

namespace mira_gui {
class GameEditForm;
class GameTileDelegate;
class HoverCard;
class SettingsPanel;
}

// Primary library view: cover-art grid, a left sidebar (filters, sort,
// search, Library/Classic-view nav, Settings), custom top bar in place of a
// native titlebar. Frameless, so it owns its own
// move/resize/minimize/maximize/close.
//
// Peer of MainWindow, not a replacement — MainWindow (table view) stays
// reachable from the sidebar's Classic table view row and `mira-gui
// --classic` for auditing a freshly scanned library. Both are thin clients
// over the same MiradClient calls.
//
// Selection model: one click selects a tile, a second (double) click
// launches, right-click opens the per-game menu. Hovering a tile shows a
// HoverCard after a short dwell — the tile itself plus the right-click menu
// and the per-game edit page cover everything the old right sidebar used to.
class LibraryWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit LibraryWindow(QWidget* parent = nullptr);

private:
  QWidget* BuildTopBar();
  QWidget* BuildSidebar();
  QWidget* BuildGrid();
  QWidget* BuildSettingsPage();
  void BuildMenus();
  void BuildShortcuts();

  // Frontend's own state (size, tile size, which filter) round-trips through
  // frontend.toml, not settings.toml. Applied after the window is already
  // up, so a slow or absent daemon costs a visible resize, not a blank window.
  void LoadPrefs();
  void SavePrefs();
  void closeEvent(QCloseEvent* event) override;
  void changeEvent(QEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void ToggleMaximize();
  // Redrawn rather than stored: each glyph is painted in the theme's text
  // color, so a theme change has to regenerate them.
  void ApplyTopBarIcons();
  void ApplyLayoutTokens();

  // `force_scan` separates the two callers: startup, which honours the
  // scan_on_startup preference, and Refresh, which does not.
  void RefreshHealth(bool force_scan = false);
  void RescanAndRefreshGames(bool force_scan);
  void RefreshGames();

  // games_ is the whole library as last fetched; the grid is a filtered
  // projection of it. Filtering client-side keeps the search box instant and
  // lets "Playing now"/"Never played" be filters at all.
  void ApplyFilter();
  bool MatchesFilter(const mira_gui::GameSummary& game) const;
  // The part of MatchesFilter that doesn't depend on the search box — shared
  // with UpdateFilterCounts, which needs every key's count, not just the
  // active one's.
  bool MatchesFilterKey(const mira_gui::GameSummary& game, const QString& key) const;
  void UpdateFilterCounts();
  QString CurrentFilterKey() const;
  void UpsertGame(const mira_gui::GameSummary& game);
  void RemoveGame(const std::string& id);
  const mira_gui::GameSummary* FindGame(const std::string& id) const;

  QPixmap CoverFor(const mira_gui::GameSummary& game);
  void SetTileWidth(int width);
  QSize TileSize() const;

  void SelectionChanged();
  // nullptr hides it; otherwise positions and fills a persistent HoverCard
  // for that tile. Called by LibraryGrid::on_hover_item after its dwell.
  void ShowHoverCard(QListWidgetItem* item);
  void ShowContextMenu(const QPoint& pos);
  // More than one tile selected — a reduced set of actions applied to all
  // of them at once, chosen at the pos the right-click landed on.
  void ShowBatchContextMenu(const QList<QListWidgetItem*>& items, const QPoint& pos);
  void ToggleRunning(const std::string& id);
  void ToggleHidden(const std::string& id);
  // Adds the hidden tag to each id that doesn't already have it — batch
  // "Hide" only ever hides, unlike the single-game toggle.
  void BatchHide(const std::vector<std::string>& ids);
  void LaunchGame(const std::string& id);
  void OpenGameDialog(const std::string& id);
  QWidget* BuildGameEditPage(const std::string& id);
  void CloseGameEdit();
  // Confirms first if game_edit_form_ is dirty — the edit page's own Back
  // button, and the sidebar's Library nav row.
  void RequestCloseGameEdit();
  // `focus_key` jumps straight to that schema field once loaded.
  void OpenSettings(const QString& focus_key = QString());
  void CloseSettings();
  // Confirms first if settings_panel_ is dirty — the top bar's Back button.
  void RequestCloseSettings();
  bool SettingsOpen() const;
  // Gear <-> Back/Save, and greys out the library controls either way.
  void SetSettingsChromeVisible(bool settings_open);
  // Shared by SetSettingsChromeVisible and the per-game edit page: neither
  // filtering nor sorting means anything while the grid isn't on screen.
  void SetGridControlsEnabled(bool enabled);
  // Highlights the sidebar's "Library" row exactly when the grid is the
  // visible content (not Settings, not a game's edit page).
  void UpdateLibraryNavActive();
  void OpenRunners();
  void OpenAbout();
  void OpenGameDetailPage(const std::string& id);
  void ScanLibrary();
  void ImportSteamLibrary();
  void ImportLutrisLibrary();
  void ImportDesktopEntries();
  void AddGameManually();
  void OpenClassicView();
  // `announce` is false for the bulk path, where one toast covers the batch
  // and per-game messages would be one notification per game.
  void RefreshMetadata(const std::string& id, bool announce = true);
  void OpenArtworkPicker(const std::string& id, const std::string& slot);
  void FetchMissingArtwork();
  void SyncDesktopEntries();
  void RemoveAllDesktopEntries();
  void ShowSteamGridDbNotice(bool asked_for);
  void UpdateTileCover(const QString& id);

  void HandleGameEvent(const std::string& type, const std::string& data);
  int FilterRow(const QString& key) const;

  QWidget* top_bar_ = nullptr;
  QToolButton* menu_button_ = nullptr;
  QLineEdit* search_ = nullptr;
  // One row per kFilters entry, each carrying its key in Qt::UserRole and a
  // live count via a custom row widget (see UpdateFilterCounts).
  QListWidget* filters_ = nullptr;
  LibraryGrid* grid_ = nullptr;
  QVBoxLayout* grid_layout_ = nullptr;
  mira_gui::GameTileDelegate* delegate_ = nullptr;
  QSlider* zoom_ = nullptr;
  QComboBox* sort_ = nullptr;
  QToolButton* sort_direction_ = nullptr;
  QToolButton* add_games_ = nullptr;
  // Sidebar row now, styled like library_nav_/classic_view_nav_ — see
  // SetSettingsChromeVisible for how it and the top bar's Back/Save pair
  // (still shown/hidden together) coordinate.
  QPushButton* settings_button_ = nullptr;
  QWidget* settings_actions_widget_ = nullptr;
  QPushButton* settings_back_button_ = nullptr;
  QPushButton* settings_reset_button_ = nullptr;
  QPushButton* settings_save_button_ = nullptr;
  QToolButton* minimize_button_ = nullptr;
  QToolButton* maximize_button_ = nullptr;
  QToolButton* close_button_ = nullptr;

  // The left sidebar's two nav rows — Library is checked/highlighted
  // whenever content_stack_ shows splitter_ (see UpdateLibraryNavActive).
  QPushButton* library_nav_ = nullptr;
  QPushButton* classic_view_nav_ = nullptr;

  QSplitter* splitter_ = nullptr;
  // Swaps the whole splitter (sidebar + grid) out for settings or a game's
  // edit page, full-screen — neither has anywhere else to go now that
  // there's no right sidebar to hold the edit page narrow next to the grid.
  QStackedWidget* content_stack_ = nullptr;
  // Rebuilt on every OpenSettings() so it starts synced to what's actually
  // saved, not stale edits left over from a discarded previous open.
  QWidget* settings_page_ = nullptr;
  mira_gui::SettingsPanel* settings_panel_ = nullptr;
  // A game's editable form, full-width in content_stack_ (game_settings_in_sidebar_
  // pref) or a modal dialog instead — see OpenGameDialog.
  QWidget* game_edit_page_ = nullptr;
  mira_gui::GameEditForm* game_edit_form_ = nullptr;
  bool game_settings_in_sidebar_ = true;
  QLabel* footer_ = nullptr;
  QLabel* empty_hint_ = nullptr;
  mira_gui::HoverCard* hover_card_ = nullptr;

  std::vector<mira_gui::GameSummary> games_;
  std::set<std::string> running_ids_;
  // Whether RefreshGames() has ever completed successfully.
  bool loaded_ = false;
  // Games the user explicitly asked to refresh — a metadata failure for one
  // of these is worth a toast; the dozens from an automatic scan are not.
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
  // Keyed by "<id>@<tile width>" — a generated cover is cheap but not free,
  // and ApplyFilter() rebuilds every visible tile on each keystroke.
  mira_gui::ArtworkStore* artwork_ = nullptr;
  mira_gui::shortcuts::Common common_;

  mira_gui::EventStream event_stream_;
};
