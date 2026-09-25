# The frontend (`mira-gui`)

`mira-gui` is a Qt6 client of the REST API in [`api.md`](api.md). It never links against `mira_core`.

## Windows

`mira-gui` opens on the **grid** (`views/LibraryWindow`): cover tiles, a left sidebar and a custom top bar in place of a native titlebar. The window is frameless. Dragging the top bar's empty area or the sidebar header moves it, double-clicking the top bar toggles maximize, and the edges resize. Moves and resizes go through `QWindow::startSystemMove`/`startSystemResize` so they work on Wayland and X11.

The **table** (`views/MainWindow`) shows every field of every game. The top bar's grid/table toggle shows it in the grid's place, filtered and sorted by the same sidebar controls. `mira-gui --classic` opens it as its own window.

### Sidebar

From top to bottom: the Mira header, the Library, Runners and Settings rows, search with the filter and sort menu, the sources, recently played games, and the Add games button.

Only sources that are set up show in the sidebar. *Manage sources* (`dialogs/ManageSourcesDialog`) lists every source, sets which ones show and their order, and opens the page of one that isn't set up yet.

### Source pages

Each source row opens `views/SourcePage` in the grid's place:

- **Stores** (Epic Games, GOG, itch.io, Amazon Games, Humble Bundle): install the helper tool, sign in by pasting what the login page shows, import installed games. Owned games that aren't installed show as a second grid with an Install button (Download for Humble).
- **Launchers** (Battle.net, Ubisoft Connect, EA app): install the launcher into its own prefix, open it, import its games.
- **Local** (Steam, Lutris): import what the other program installed. Steam also lists owned games once a Web API key is set.

A page shows a banner with the source's status, a setup card while a step is left, and the source's games as tiles (`ui/TileGrid`). Covers for games that aren't installed come from `/v1/library/artwork`. Login URLs and paste parsing come from `mirad`, so the page only holds wording. A source turned off with `<id>.enabled` isn't listed.

### Runners page

`views/RunnersPage` has a Proton/Wine switch that filters both of its lists:

- **Installed** builds show their label and source. Builds Mira downloaded can be updated or removed; distro and Steam builds show as managed outside Mira. After an update it offers to remove the old build. *Make default* and *Pick automatically instead* set `default_runner.windows`.
- **Get more** lists releases from the chosen source, with an Install button on each.

A banner offers to install umu-launcher or winetricks when either is missing.

### Downloads

The top bar's download button opens `ui/DownloadsPanel`. It lists everything `ui/DownloadTracker` has seen from the event stream: game installers, store installs and updates, Humble downloads, launcher installs, tool downloads and runner downloads. `mirad` replays recent events on connect, so work started before the GUI opened also shows. Only game installers report progress (bytes written). A finished install offers *Show*, which selects the game.

### Selection and hover

One click selects a tile. Ctrl, Shift or a drag selects several, and the context menu then offers batch actions. Double-click launches. A single click never launches, so a misclick can't start a game. *Drag to select* on the Interface tab turns drag selection off.

Hovering a tile shows `ui/HoverCard` with the name, status, runner, ProtonDB tier, developer and genres. Tooltips everywhere go through `ui/ToolTip`, which draws them as the same card.

### Keyboard

`ui/KeyBindings` holds every shortcut, and each can be changed in Settings (stored in `shortcut_overrides`). `ui/Shortcuts` installs `Ctrl+Q`, `Ctrl+W` and `F1` (list of shortcuts) on both windows. The grid adds `Ctrl+F` search, `Esc`, `Ctrl+1` to `Ctrl+8` filters, `F5`/`Ctrl+R` refresh, `Ctrl+,` settings, tile size keys and, while the grid has focus, `Enter` to play or stop, `Alt+Enter` for details and `Delete` to remove. That last group is scoped to the grid so the keys still work in the search box.

Quit goes through `QApplication::closeAllWindows()` so `LibraryWindow::closeEvent` saves its prefs.

## Theming

`ui/Theme` sets the Fusion style, palette and stylesheet at startup instead of inheriting the desktop's.

A theme is a TOML file of tokens. `themes/base.qss` is the only stylesheet, and every `@token` in it is filled from the current theme. `mira-dark` and `mira-light` are built in; user themes go in `$XDG_CONFIG_HOME/mira/themes/*.toml`. Missing or invalid keys fall back to the defaults. The `theme` pref is a theme name or `auto`, which follows the desktop's light/dark setting.

`theme::Overrides` applies tile spacing, grid padding and corner radii from `frontend.toml` on top of any theme. `-1` means "use the theme's value".

Widgets that paint by hand read tokens directly and repaint on `theme::Notifier::Changed`: `ui/GameTileDelegate`, `ui/CoverArt`, `ui/Notify`, `ui/HeroArtWidget` and `ui/HeroBackdrop`.

Widgets don't set colors themselves. They set a style property (`setProperty("role", "muted")`) and `base.qss` styles it; `theme::SetStyleProperty` re-polishes after a change. `ui/Icons` draws icons as vector paths in the theme's text color.

## Layers

```
client/   talks to mirad, draws nothing
ui/       widgets shared by several views or dialogs
views/    the library windows and the pages shown in them
dialogs/  modal editors
```

Each layer only depends on the ones above it in this list.

| `client/` file | Role |
|---|---|
| `Transport` | One socket round trip: timeouts and the error envelope. |
| `JsonMapping` | JSON to and from the structs in `Types.h`. |
| `Async` | Runs a call on a worker thread and delivers the result on the main thread. |
| `MiradClient` | One method per endpoint. |
| `EventStream` | The `GET /v1/events` connection, reconnecting with `Last-Event-ID`. |
| `Types.h` | Plain data the UI depends on. |

`async::Deliver` posts results to `qApp` and checks a `QPointer` to the requesting widget on the main thread. Posting to the widget itself would read a possibly deleted object on the worker thread.

`LibraryWindow` fetches the whole library once and filters it on the client, which keeps search instant and makes "Playing now" and "Never played" possible. Events then patch the view directly. There is no polling.

Only `game.added` and `game.updated` carry a game record, so views check the event type before parsing one. `game.state` carries only the state and launch details, so an exit triggers a re-fetch. A game handed to Steam or a launcher reports `tracked` in the launch reply and `game.launched`, and isn't marked running unless it is tracked.

## Settings

- **`settings.toml`** holds backend settings. `ui/SettingsPanel` is generated from `GET /v1/config/schema`: sections, order, labels, dividers and runner pickers (`is_runner_ref`) all come from the schema, so a new backend setting needs no frontend change.
- **`frontend.toml`** holds GUI state and preferences (`FrontendPrefs` in `client/Types.h`), read and written as the `frontend` key of `/v1/config`. The backend never validates it.

| Key | Meaning |
|---|---|
| `window_width`, `window_height` | Window size. |
| `sidebar_width` | Sidebar width. |
| `tile_width` | Tile size, clamped to the zoom slider's range. |
| `library_filter`, `sort_by`, `sort_descending` | Selected filter and sort. |
| `scan_on_startup` | Run a library scan when the GUI opens. |
| `theme` | Theme name or `auto`. |
| `game_settings_in_sidebar` | Edit a game in the side panel instead of a dialog. |
| `drag_select` | Drag across the grid to select. |
| `tile_spacing`, `grid_margin`, `tile_radius`, `panel_radius`, `control_radius` | Shape overrides in pixels, `-1` for the theme's value. |
| `shortcut_overrides` | Changed shortcuts by id. |
| `hidden_sources`, `source_order` | Which sources the sidebar shows, and in what order. |
| `source_imported_at` | When each source last imported. |
| `sidebar_recent_count`, `sidebar_source_counts` | How many recently played games to list, and whether source rows show counts. |

Every field is optional; a missing one means the default. Values go through the widget that owns them, so a hand-edited value is still clamped.

## Notifications

`ui/Notify` handles every message. Failures (`Failed`, `FailedWithHint`, `FailedWithAction`, `Warn`) are persistent desktop notifications; confirmations (`Notice`) use the desktop's default timeout. Only questions (`Confirm`, `ConfirmUnsaved`) and answers to a button (`Info`) are popups. Pages with their own status line, such as `RunnersPage` and `ArtPickerPanel`, report errors there.

Success that already shows on screen gets no message. A notice only goes out when nothing in the window would change, such as "No new Steam games found."

Notifications use `org.freedesktop.Notifications` (`ui/SystemNotifier`) with a `desktop-entry` hint of `mira`. `FailedWithAction` adds a button that raises the window and runs the action. Without a notification service, failures become popups and notices become cards in the window's corner.

Messages are plain text, since `mirad`'s errors quote paths and commands. Failures show `mirad`'s message under a sentence naming what failed.

## Cover art

`ui/ArtworkStore` fetches art from `GET /v1/games/{id}/artwork` and always returns a pixmap:

- A 404 is normal. `ui/CoverArt` draws a placeholder with a hue from the game's id, so it stays the same across restarts.
- Each game is asked once, until `game.metadata_ready` or a refresh.
- At most four requests are in flight.
- The original image is kept and scaled on demand for the zoom slider.

Without `steamgriddb.api_key`, non-Steam games may have no source. `mirad` then fails the fetch with `no_steamgriddb_key`, and the GUI says so once per session: a popup offering Settings when the user asked, a notice after a background scan.

Metadata is only fetched when a game is first added. A tile's *Refresh metadata && cover art* and *Fetch missing cover art* ask again.

`ui/ArtPickerPanel` is the game card's *Change hero* / *Change cover*. It shows a slot's candidates as a grid of previews with style filters, loading SteamGridDB pages as the grid scrolls. `mirad` downloads the previews (`POST .../artwork/thumbs`) only for what is on screen plus one screen ahead, and deletes them when the GUI quits. Clicking a preview shows it on the card; *Use* applies it. The *Art from* menu picks which SteamGridDB game the art comes from.

## API use

| Endpoints | Where |
|---|---|
| `GET /v1/health` | Online/Offline badge |
| `GET /v1/games`, `GET`/`PATCH /v1/games/{id}` | Library views, `ui/GameEditForm` |
| `DELETE /v1/games/{id}` | `DeleteGameDialog` |
| `GET`/`PATCH /v1/games/{id}/config` | `OverridesEditor` |
| `POST /v1/games/{id}/launch`, `/stop` | Play/Stop, double-click, context menu |
| `POST /v1/games/manual` | `AddManualGameDialog` |
| `POST /v1/games/{id}/run` | `RunInPrefixDialog` |
| `GET /v1/games/{id}/installer`, `POST .../install`, `GET .../install/progress`, `POST .../finish-install` | `InstallGameDialog`, *Mark as installed*, `DownloadTracker` |
| `POST /v1/games/{id}/tricks` | `WinetricksDialog` |
| `GET /v1/games/{id}/log` | `LogViewerDialog` |
| `POST /v1/games/{id}/relocate`, `/v1/library/relocate` | *Move to Mira's folders…* |
| Metadata and artwork endpoints | `ArtworkStore`, `HoverCard`, `GameDetailPageDialog`, `ArtPickerPanel` |
| `POST /v1/library/scan` | Startup and *Refresh library* |
| `/v1/library`, `/v1/library/install`, `/update`, `/artwork` | `SourcePage` |
| `/v1/{epic,gog,itch,amazon,humble}/*`, `/v1/launchers/*`, `/v1/sources/*` | `SourcePage`, `ItchCollectionsDialog` |
| `POST /v1/steam/scan`, `/v1/lutris/import` | Steam and Lutris pages |
| `/v1/desktop-entries/*` | `DesktopEntryImportDialog`, Settings |
| `/v1/config`, `/v1/config/schema`, `/reset` | `SettingsPanel`, `OverridesEditor`, `RunnersPage` |
| `/v1/runners/*` | `RunnersPage`, runner pickers |
| `GET /v1/gamemode/status` | Settings |
| `GET /v1/events` | `EventStream` |

## Running against a dev daemon

Run a separate daemon on its own socket and point the GUI at it:

```sh
XDG_CONFIG_HOME=~/.mira-dev mirad --socket ~/.mira-dev/mirad.sock &
MIRA_SOCKET=~/.mira-dev/mirad.sock build/dev/frontend/mira-gui
```

`XDG_CONFIG_HOME` only separates config, not the socket, so without `--socket` the dev daemon would take over the default one. The window footer shows which socket is in use.
