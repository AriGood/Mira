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
class GameDetailsPanel;
class GameEditForm;
class GameTileDelegate;
class SettingsPanel;
}

// Primary library view: cover-art grid, details panel, custom top bar in
// place of a native titlebar. Frameless, so it owns its own
// move/resize/minimize/maximize/close.
//
// Peer of MainWindow, not a replacement — MainWindow (table view) stays
// reachable from the View menu and `mira-gui --classic` for auditing a
// freshly scanned library. Both are thin clients over the same MiradClient
// calls.
//
// Selection model: one click selects a tile and fills the details panel, a
// second (double) click launches, right-click opens the per-game menu.
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
  // `focus_key` jumps straight to that schema field once loaded.
  void OpenSettings(const QString& focus_key = QString());
  void CloseSettings();
  // Confirms first if settings_panel_ is dirty — the top bar's Back button.
  void RequestCloseSettings();
  bool SettingsOpen() const;
  // Gear <-> Back/Save, and greys out the library controls either way.
  void SetSettingsChromeVisible(bool settings_open);
  void OpenRunners();
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
  QComboBox* filters_ = nullptr;
  LibraryGrid* grid_ = nullptr;
  QVBoxLayout* grid_layout_ = nullptr;
  mira_gui::GameTileDelegate* delegate_ = nullptr;
  QSlider* zoom_ = nullptr;
  QComboBox* sort_ = nullptr;
  QToolButton* sort_direction_ = nullptr;
  QToolButton* add_games_ = nullptr;
  // The gear and the Back/Save pair are siblings, one shown at a time — see
  // SetSettingsChromeVisible.
  QToolButton* settings_button_ = nullptr;
  QWidget* settings_actions_widget_ = nullptr;
  QPushButton* settings_back_button_ = nullptr;
  QPushButton* settings_reset_button_ = nullptr;
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
