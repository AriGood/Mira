#pragma once

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../client/EventStream.h"
#include "../client/Types.h"
#include "../ui/ArtworkStore.h"
#include "../dialogs/ManageSourcesDialog.h"
#include "../ui/Shortcuts.h"

class QLabel;
class QMenu;
class QLineEdit;
class QGridLayout;
class QListWidget;
class QPushButton;
class QSlider;
class QSplitter;
class QStackedLayout;
class QStackedWidget;
class QVBoxLayout;
class QAction;
class QToolButton;
class QListWidgetItem;
class QTableWidget;
class QTimer;

// QListWidget with setViewportMargins made public — Qt keeps it protected on
// QAbstractScrollArea. Defined in LibraryWindow.cpp; this file only ever
// holds a pointer to it.
class LibraryGrid;

namespace mira_gui {
class DownloadTracker;
class DownloadsPanel;
class GameEditForm;
class GameTileDelegate;
class HoverCard;
class SettingsPanel;
class SourcePage;
struct SourceInfo;
}

// Primary library view: cover-art grid, a left sidebar (filters, sort,
// search, Library/Classic-view nav, Settings), custom top bar in place of a
// native titlebar. Frameless, so it owns its own
// move/resize/minimize/maximize/close.
//
// `mira-gui --classic` still opens MainWindow standalone; the top bar's
// table toggle instead shows the same table in the grid's place, reading
// this window's own games_/running_ids_.
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
  // The sidebar's single filter+sort control, a Qt::Popup so it dismisses
  // itself on an outside click or Escape — no manual close-on-click-away
  // wiring needed. Built once; filters_ and the sort buttons live inside it.
  QWidget* BuildFilterSortPopover();
  // Refreshes the pill's own summary text/icons after a filter, sort, or
  // theme change — the popover's own rows restyle themselves separately.
  void UpdateFilterSortSummary();
  // Library-only actions as vertical icon+label rows. Refresh/Shortcuts/
  // About moved to the top bar; Close window/Quit dropped (the frameless ×
  // and the tray icon already cover them).
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
  // `anchor` is global; the card goes beside it.
  void ShowHoverCardFor(const mira_gui::GameSummary& game, const QRect& anchor,
                        const QString& hint = QString());
  void ShowContextMenu(const QPoint& pos);
  // `extra` adds entries after Play (e.g. a store's Update).
  void ShowGameMenu(const std::string& id, const QPoint& global_pos,
                    const std::function<void(QMenu&)>& extra = nullptr);
  void ShowSidebarMenu(const QPoint& global_pos);
  void ShowSourceMenu(const mira_gui::SourceInfo& source, const QPoint& global_pos);
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
  // Scrim + centered card slot, built once. Shown/hidden per open rather
  // than swapped into content_stack_, so the grid and sidebar stay live
  // underneath it.
  QWidget* BuildGameEditOverlay();
  // The card's own content, rebuilt fresh on every open — same reasoning as
  // settings_page_: starts synced to what's actually saved, not stale edits
  // from a discarded previous open.
  QWidget* BuildGameEditCard(const std::string& id);
  void CloseGameEdit();
  // Confirms first if game_edit_form_ is dirty — the card's own Back
  // button, the sidebar's Library nav row, and a click on the scrim.
  void RequestCloseGameEdit();
  bool GameEditOpen() const;
  // `focus_key` jumps straight to that schema field once loaded.
  void OpenSettings(const QString& focus_key = QString());
  void CloseSettings();
  // Confirms first if settings_panel_ is dirty — the settings page's own
  // Back button.
  void RequestCloseSettings();
  bool SettingsOpen() const;
  // Greys out the library controls behind Settings while it's open.
  void SetSettingsChromeVisible(bool settings_open);
  // Shared by SetSettingsChromeVisible and the per-game edit page: neither
  // filtering nor sorting means anything while the grid isn't on screen.
  void SetGridControlsEnabled(bool enabled);
  // Highlights the sidebar's "Library" row exactly when the grid is the
  // visible content (not Settings, not a game's edit page).
  void UpdateLibraryNavActive();
  void ShowLibrary();
  void OpenManageSources();
  void RefreshRecentlyPlayed();
  void SetSourceHidden(const QString& id, bool hidden);
  std::vector<QString> SourceOrder() const;
  std::vector<ManageSourcesDialog::Entry> SourceEntries() const;
  void MoveSourceBy(const QString& id, int delta);
  void NoteImported(const QString& id);
  // Moves `id` to just before the visible row `before` (end if -1).
  void MoveSource(const QString& id, int before);
  int SourceDropRow(int y) const;
  void OpenRunners();
  void OpenAbout();
  void OpenGameDetailPage(const std::string& id);
  void ScanLibrary();
  void ImportSteamLibrary();
  void ImportLutrisLibrary();
  void ImportDesktopEntries();
  void AddGameManually();
  QWidget* BuildClassicPage();
  // Repopulates classic_table_ from the same filtered games_ the grid just
  // rebuilt — called at the end of ApplyFilter so the two views never drift.
  void RefreshClassicTable();
  void OpenClassicView();
  void CloseClassicView();
  // A store or launcher's page, rebuilt fresh on each open.
  void OpenSource(const mira_gui::SourceInfo& source);
  void CloseSource();
  // Hides the sources turned off in Settings (`<id>.enabled`), and asks
  // which stores are signed in and which launchers installed.
  void RefreshSourceNavs();
  // Greys out and moves down the sources with nothing set up yet.
  void UpdateSourceNavs();
  void SetSourceControlsEnabled(bool enabled);
  // The grid is what's on screen: not Settings, the classic table, or a source page.
  bool GridShown() const;
  bool ClassicShown() const;
  void RelocateLibrary();
  // A download or install moved along: tile text and the top bar's count.
  void DownloadChanged(const QString& key);
  // Back to the grid with this game selected; its settings if filtered out.
  void ShowGame(const std::string& id);
  // "Installing… 1.2 GB" for a game mid-install, else empty.
  QString InstallText(const std::string& id) const;  // `announce` is false for the bulk path, where one toast covers the batch
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
  QLineEdit* search_ = nullptr;
  // One row per kFilters entry, each carrying its key in Qt::UserRole and a
  // live count via a custom row widget (see UpdateFilterCounts) — lives
  // inside filter_sort_popover_, not directly in the sidebar layout.
  QListWidget* filters_ = nullptr;
  // The sidebar's always-visible filter+sort pill; concrete type (a small
  // QWidget subclass with a plain on_clicked callback, matching LibraryGrid's
  // own pattern) is local to LibraryWindow.cpp.
  QWidget* filter_sort_button_ = nullptr;
  QLabel* filter_icon_ = nullptr;
  QLabel* filter_summary_label_ = nullptr;
  QLabel* sort_icon_ = nullptr;
  QLabel* sort_summary_label_ = nullptr;
  QLabel* filter_sort_chevron_ = nullptr;
  QWidget* filter_sort_popover_ = nullptr;
  // One button per mira_gui::SortOptions() entry, exclusive selection —
  // replaces the old QComboBox with a vertical list of full-width rows.
  QList<QPushButton*> sort_buttons_;
  LibraryGrid* grid_ = nullptr;
  QVBoxLayout* grid_layout_ = nullptr;
  mira_gui::GameTileDelegate* delegate_ = nullptr;
  QSlider* zoom_ = nullptr;
  QToolButton* sort_direction_ = nullptr;
  QToolButton* add_games_ = nullptr;
  // Sidebar nav row, styled like library_nav_. Settings' own
  // Back/Reset/Save row lives on the settings page itself (BuildSettingsPage),
  // rebuilt fresh alongside settings_panel_ on each open.
  QPushButton* settings_button_ = nullptr;
  // Moved here from the sidebar's old hamburger menu — see BuildTopBar.
  QToolButton* downloads_button_ = nullptr;
  QToolButton* refresh_button_ = nullptr;
  QToolButton* shortcuts_button_ = nullptr;
  QToolButton* about_button_ = nullptr;
  QWidget* top_bar_divider_ = nullptr;
  QToolButton* minimize_button_ = nullptr;
  QToolButton* maximize_button_ = nullptr;
  QToolButton* close_button_ = nullptr;

  // Library is checked/highlighted whenever content_stack_ shows splitter_
  // (see UpdateLibraryNavActive).
  QPushButton* library_nav_ = nullptr;
  QPushButton* runners_nav_ = nullptr;
  QToolButton* manage_sources_button_ = nullptr;
  QToolButton* fetch_art_button_ = nullptr;
  // Top bar grid/table switch, in place of the old classic view row.
  QWidget* view_toggle_ = nullptr;
  QToolButton* grid_view_button_ = nullptr;
  QToolButton* table_view_button_ = nullptr;
  // One sidebar row per mira_gui::AllSources() entry, same order; only set up
  // sources that aren't hidden are visible.
  QList<QPushButton*> source_navs_;
  QList<QLabel*> source_counts_;
  QSet<QString> hidden_sources_;    // unticked "In sidebar"
  QSet<QString> disabled_sources_;  // <id>.enabled = false
  std::vector<QString> source_order_;  // saved order; see SourceOrder()
  QHash<QString, QString> source_account_;      // signed-in account, where a store says
  QHash<QString, qint64> source_imported_at_;   // last import, unix seconds
  QWidget* source_nav_container_ = nullptr;  // accepts source row drops
  QWidget* source_drop_line_ = nullptr;
  QPushButton* source_drag_row_ = nullptr;
  QPoint source_drag_start_;
  QLabel* recent_heading_ = nullptr;
  QVBoxLayout* recent_layout_ = nullptr;
  static constexpr int kDefaultRecentCount = 3;
  int recent_count_ = kDefaultRecentCount;  // besides running games
  bool show_source_counts_ = true;
  QVBoxLayout* source_nav_layout_ = nullptr;
  // Store signed in / launcher installed, by source id, as last asked.
  QHash<QString, bool> source_ready_;
  mira_gui::SourcePage* source_page_ = nullptr;

  QSplitter* splitter_ = nullptr;
  // The splitter's right side: grid_page_, classic_page_, or source_page_.
  QStackedWidget* main_stack_ = nullptr;
  QWidget* grid_page_ = nullptr;
  // Swaps the splitter out for Settings, full-screen. A
  // game's edit card is a separate overlay (game_edit_overlay_) that stays
  // over the grid instead.
  QStackedWidget* content_stack_ = nullptr;
  // Rebuilt on every OpenSettings() so it starts synced to what's actually
  // saved, not stale edits left over from a discarded previous open.
  QWidget* settings_page_ = nullptr;
  mira_gui::SettingsPanel* settings_panel_ = nullptr;
  // A game's editable form, in a centered overlay card (game_settings_in_sidebar_)
  // or a modal dialog — see OpenGameDialog. The overlay is a chrome sibling,
  // not a content_stack_ page, so the grid stays visible (dimmed) underneath.
  QWidget* game_edit_overlay_ = nullptr;
  QGridLayout* game_edit_overlay_layout_ = nullptr;
  QWidget* game_edit_card_ = nullptr;
  // Owns chrome (top_bar_ + content_stack_) at index 0 and game_edit_overlay_
  // at index 1 -- StackAll shows both always; this just decides which one is
  // raised on top, toggled in OpenGameDialog/CloseGameEdit.
  QStackedLayout* root_stack_ = nullptr;
  mira_gui::GameEditForm* game_edit_form_ = nullptr;
  bool game_settings_in_sidebar_ = true;
  // Built once at startup, not per-open like settings_page_/game_edit_card_
  // — it has no per-session state to go stale, so it just stays synced via
  // RefreshClassicTable().
  QWidget* classic_page_ = nullptr;
  QTableWidget* classic_table_ = nullptr;
  QLabel* footer_ = nullptr;
  QLabel* empty_hint_ = nullptr;
  mira_gui::HoverCard* hover_card_ = nullptr;
  // Dwell before a recently played row's hover card.
  QTimer* recent_hover_ = nullptr;
  QPointer<QWidget> recent_hover_row_;

  std::vector<mira_gui::GameSummary> games_;
  std::set<std::string> running_ids_;
  mira_gui::DownloadTracker* downloads_ = nullptr;
  mira_gui::DownloadsPanel* downloads_panel_ = nullptr;
  // game.added events asking for their settings to open, gathered briefly
  // so a scan's burst of them opens nothing.
  std::vector<std::string> pending_added_;
  QTimer* added_timer_ = nullptr;
  // Whether RefreshGames() has ever completed successfully.
  bool loaded_ = false;
  // Games the user explicitly asked to refresh — a metadata failure for one
  // of these is worth a toast; the dozens from an automatic scan are not.
  std::set<std::string> awaiting_metadata_;
  bool steamgriddb_notice_shown_ = false;
  // Set by "Fetch missing cover art": its fetches were asked for, so a
  // missing SteamGridDB key is worth reporting.
  bool artwork_fetch_requested_ = false;
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
