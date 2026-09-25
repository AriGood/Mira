# The frontend (`mira-gui`)

What the Qt frontend does, how it is put together, and which parts of
[`api.md`](api.md) it uses. For *why* the frontend is a REST client rather
than a second copy of the backend, see [`architecture.md`](architecture.md);
that decision is load-bearing and is not restated here.

## Two views, both kept

`mira-gui` opens on **the grid** (`views/LibraryWindow`): cover tiles and a
left sidebar (filters, sort, search, Library navigation, a
Sources list, Settings, and a status line), with a custom top bar in place of a native
titlebar — Add/Import Games, tile size, and window controls (minimize/maximize/
close), inspired by Lutris. It is modelled on Playnite's shelf for the tile
browsing itself, which is the interaction most people arriving at a Linux
game launcher already know.

The window is frameless (`Qt::FramelessWindowHint`): the top bar owns
move (`QWindow::startSystemMove`, dragging its own empty background) and
double-click-to-maximize, and a `RootWidget` central widget reserves a few
pixels at each edge for `QWindow::startSystemResize`. Both go through the
compositor rather than repositioning the window by hand, which is what makes
them work under Wayland as well as X11.

**The table** (`views/MainWindow`) shows every field of every game at once.
It is reachable as `mira-gui --classic`. The top bar's grid/table toggle shows
the same table in the grid's place, next to the sidebar, filtered and sorted
by the same sidebar controls; clicking *Table* again goes back to the grid.

### Sources

The sidebar's *Sources* rows each open `views/SourcePage` in the grid's
place, with the sidebar still up. There are three kinds:

- **Stores** (Epic Games, GOG, itch.io, Amazon Games, Humble Bundle): download
  the helper tool mirad drives (Legendary, gogdl, butler, nile, humble-cli),
  sign in by pasting what the store's login page shows, import what's
  installed. What the account owns but hasn't installed is a second tile grid
  with an Install pill on each tile (Humble: Download). Its covers are fetched
  when the page lists them, from the store's own art where mirad can find it.
  A title with none needs a SteamGridDB key, and the page offers a button to
  Settings when one is missing.
- **Launchers** (Battle.net, Ubisoft Connect, EA app): install the launcher
  into its own prefix, open it, import the games installed through it.
- **Local** (Steam, Lutris): import what the other program installed. Steam
  also lists owned games once a Web API key is set.

Every page opens on a banner in the source's color with its status, then the
games from that source (`GameSummary::source`) as cover tiles
(`ui/TileGrid`, which grows to fit instead of scrolling). A setup card appears
only while a step is left. A tile not yet installed gets its cover from
`/v1/library/artwork`, the placeholder until that arrives (Humble bundles
always: they aren't games). A source turned off in Settings (`<id>.enabled`) is left out of the list. Paste
parsing and login URLs come from mirad, so the page holds only wording.

### Downloads

The top bar's download button (with a count while anything runs) opens
`ui/DownloadsPanel`, listing what `ui/DownloadTracker` has seen, newest
first: game installers, store installs and updates, Humble downloads,
launcher installs, store helper downloads and runner downloads. The tracker
is fed from the library window's event stream, which mirad replays on
connect, so work started before the GUI shows too. Only game installers
report progress (bytes written); the rest show a busy bar until they finish
or fail. A finished install offers *Show*, which selects the game in the
grid. Source pages read the tracker too, so an Install pill still says
"Installing…" after leaving and reopening the page.

Neither is a fallback for the other. The grid is the better browser; the
table is the better audit tool for a library that was just scanned, where
the question is "what did detection get wrong" and the answer is a column.
Both are thin clients over the same `MiradClient` calls and each holds its
own `EventStream`, so two open windows cannot disagree about what the daemon
said.

### Selection model

One click selects a tile (or, with Ctrl/Shift or a drag, several — the
context menu then offers a reduced batch version of its usual actions). A
drag that starts on a tile only begins once the cursor leaves that tile, and
it scrolls the grid near the top or bottom edge. *Drag to select* on the
Interface tab turns it off. A
second (double) click launches. Right-click opens the per-game menu.
Launching on the first click would turn a misclick into a started game, so a
single click never launches anything.

Hovering a tile for ~280ms shows `ui/HoverCard`, a floating, non-modal
preview built from the `GameSummary` already in memory (name, status,
platform/runner) plus one `GET /v1/games/{id}/metadata` call for the
ProtonDB tier and developer/genres — the old right sidebar's job, without a
permanent panel taking up space. It never shows over a multi-selection.
Store pages' tiles and the sidebar's recently played rows show the same card
(a not-installed title gets its name, state and store).

Every other tooltip goes through `ui/ToolTip`, an app-wide event filter that
draws it as the same card, under the hovered widget (beside a menu item)
rather than at the cursor. Menu actions' own tooltips show too.
`ui/AboutPanel` (logo, version, authors, repository, license) moved to
*Help → About Mira* — a frameless window has no native Help menu to hang a
dialog off, so it's a plain `QDialog` built in `LibraryWindow::OpenAbout`.

### Keyboard

`Ctrl+Q` quits, `Ctrl+W` closes one window, `F1` lists every key the focused
window responds to. Those three are installed by `ui/Shortcuts` on both
views, so the classic table — which has no menu bar to hang an action on —
still has them.

Quit goes through `QApplication::closeAllWindows()` rather than `quit()`,
because `LibraryWindow` saves its layout in `closeEvent` and a quit that
skipped that handler would drop the prefs silently.

The grid adds: `Ctrl+F` search, `Esc` (clears the search first, the
selection second), `Ctrl+1`–`Ctrl+8` filters, `F5`/`Ctrl+R` refresh,
`Ctrl+,` settings, `Ctrl++`/`Ctrl+-`/`Ctrl+0` tile size, and — only while
the grid itself has focus — `Enter` to play or stop, `Alt+Enter` for details,
`Delete` to remove.

That last group is scoped `Qt::WidgetWithChildrenShortcut` rather than to the
window. `Delete` and `Enter` have to keep meaning what they mean inside the
search box, and window-scoped actions would swallow them: typing a game's
name and pressing Backspace-Delete would otherwise open the remove prompt for
whatever tile happened to be selected.

`Ctrl+Q` and `Ctrl+,` are spelled out rather than taken from
`QKeySequence::Quit`/`::Preferences`. Qt binds `Preferences` on macOS only,
so the Settings row showed no shortcut at all on Linux.

## Theming

`mira-gui` sets its own style, palette and stylesheet at startup
(`ui/Theme.cpp`), rather than inheriting the desktop's. Fusion is the base
style: it is fully palette-driven and identical everywhere, so our stylesheet
is not layering over Breeze or Adwaita and inheriting whatever they drew for
the parts it does not name.

A **theme is a TOML token file**, never raw selectors. One stylesheet ships
with the app (`themes/base.qss`) and every `@token` in it is substituted from
the theme being applied, so a user theme cannot break when a widget is renamed
or restructured. Bundled themes (`mira-dark`, `mira-light`) are embedded in the
binary; a user's own go in `$XDG_CONFIG_HOME/mira/themes/*.toml` and show up in
the settings picker. Every key is optional — anything missing, misspelled or of
the wrong type falls back to the built-in default rather than failing the file,
so a half-written theme still produces a usable window.

The `theme` preference is a theme name or `auto`, which follows the desktop's
own light/dark setting (`QStyleHints::colorScheme`) and keeps following it.

**Shape is adjustable without writing a theme.** `theme::Overrides` carries a
handful of pixel values from `frontend.toml` — tile spacing, grid padding and
the tile, panel and control corner radii — and is applied on top of whatever
theme is current, so switching theme keeps them. They are preferences about
one library rather than part of a theme's identity, which is why they live in
`frontend.toml`. `-1` is how the file spells "leave it to the theme": the keys
have to stay writable to be cleared again, and a merge patch cannot drop one.

Five things the stylesheet cannot reach read the tokens directly instead:
`ui/GameTileDelegate` and `ui/CoverArt`, which paint with `QPainter`,
`ui/Notify`'s toasts, `ui/HeroArtWidget`'s banner, which is clipped to
`radius_panel` by hand because a stylesheet cannot round a pixmap inside a
`QLabel`, and `ui/HeroBackdrop`, the game settings card, which paints the
game's hero (or its cover, blurred) across its top and fades it into
`surface`. They repaint on `theme::Notifier::Changed`.

Two conventions keep colors out of the widgets themselves:

- **Style properties, not per-widget stylesheets.** A label says what it *is*
  (`setProperty("role", "muted")`, or `"status"` for a lifecycle color) and
  `base.qss` says what that looks like. `theme::SetStyleProperty` re-polishes
  the widget, which Qt does not do on its own when a property changes after
  the widget has been polished.
- **`ui/Icons`** draws the top bar's glyphs (menu, gear, minimize, maximize,
  restore, close) as vector paths in the theme's text color, rather than
  `QStyle::standardIcon` — those are the platform style's dated titlebar
  buttons and take their color from the platform.

## Layers

Four directories, depending only downward:

```
client/   talks to mirad, draws nothing
ui/       presentation shared by more than one view or dialog
views/    the two top-level library windows
dialogs/  the modal editors
```

### `client/`

| file | what it owns |
|---|---|
| `Transport` | one socket round trip: timeouts, the `{"error": {...}}` envelope |
| `JsonMapping` | JSON ↔ the structs in `Types.h`; no I/O, so this is what to read when a field looks wrong |
| `Async` | the worker-thread hop, and the liveness rule that makes it safe |
| `MiradClient` | one method per endpoint, and nothing else |
| `EventStream` | the long-lived `GET /v1/events` connection |
| `Types.h` | the plain data the UI is allowed to depend on |

**Threading.** Every `MiradClient` call runs on a throwaway thread and
delivers its result back on the main thread. The delivery goes through
`async::Deliver`, which posts to `qApp` and tests a `QPointer` on the main
thread — *not* to the calling widget.

This is not a stylistic choice. `QMetaObject::invokeMethod` dereferences its
context argument on the **calling** thread, because it has to read that
object's thread affinity, so passing a `QObject*` that the main thread may
already have deleted is a use-after-free before the queued call is ever
posted. That crashed `mira-gui` reproducibly whenever a window went away
with a request or an event in flight. `qApp` outlives every window, and the
`QPointer` is tested on the same thread that destroys widgets, so it cannot
go stale between the check and the call.

**Filtering is client-side.** `LibraryWindow` fetches the whole library once
and filters in `ApplyFilter`. That is what makes the search box feel
instant, and it is the only way "Playing now" and "Never played" can be
filters at all — neither is a server-side query.

**No reconciliation poll.** Live updates arrive as events and patch the view
directly from each event's own payload. There is deliberately no periodic
re-fetch: an occasional missed event is cheaper than a timer that wakes up
forever on the off chance one was dropped. See "Idle cost" in
`architecture.md`.

## Settings live in two different files

This matters more than it looks:

- **`settings.toml`** holds backend settings. Every key is declared in
  `src/config/Schema.cpp`, and `ui/SettingsPanel` is generated entirely from
  `GET /v1/config/schema` — almost no setting name is hardcoded in the
  frontend. Adding a backend setting requires no frontend change, and where
  it shows (section, order, label, divider, per-game or not) is decided in
  the schema too.
- **`frontend.toml`** holds the frontend's own state and preferences:
  window size, tile size, which filter and sort were selected,
  the left sidebar's splitter width, and whether to scan the library on
  startup. The backend stores it verbatim and never validates it, reachable
  as the opaque `frontend` key of `GET`/`PATCH /v1/config` (see
  `FrontendPrefs`).

  | key | what it does |
  |---|---|
  | `window_width`, `window_height` | remembered window size |
  | `sidebar_width` | remembered splitter width for the left sidebar |
  | `tile_width` | cover tile size (clamped to the zoom slider's range) |
  | `library_filter` | which filter was selected |
  | `sort_by`, `sort_descending` | grid order — see `ui/LibrarySort` |
  | `scan_on_startup` | whether opening the frontend runs `POST /v1/library/scan` |
  | `theme` | a theme name, or `auto` to follow the desktop — see "Theming" |
  | `tile_spacing`, `grid_margin` | grid layout, in pixels; `-1` means "leave it to the theme" |
  | `tile_radius`, `panel_radius`, `control_radius` | corner rounding, same `-1` rule |
  | `drag_select` | whether dragging across the grid selects tiles (default on) |

  `scan_on_startup`, `theme`, `drag_select` and
  the five shape keys get rows in the settings screen, on an Interface tab
  ahead of the schema-driven ones.
  The rest are implicit UI state: they are saved by using the window, not by
  filling in a form.

A window size is not something mirad should have an opinion about, so it
never goes in `settings.toml`, where the schema would have to answer for it.
Equally, the settings screen never shows the `frontend` table: it renders
schema keys, and nothing in there is one.

`FrontendPrefs` fields are all optional. The file may be absent, partial or
hand-edited, and an unset field means "use the built-in default", not zero.
Values are applied through the widget that owns them (the tile size goes
through the zoom slider, so its range clamps a hand-edited value) rather
than assigned raw.

The one deliberate exception to "no hardcoded setting names" is
`dialogs/SettingsCategories`: it groups schema keys into sections and names
`default_runner.windows` as wanting a runner picker, because the schema has
no way to say "this string is a runner reference". A key added later with no
entry there still lands somewhere sensible via a dotted-prefix guess, and
never disappears.

## API coverage

Everything `api.md` marks implemented has a path through the UI:

| endpoint | where |
|---|---|
| `GET /v1/health` | the Online/Offline badge |
| `GET /v1/games[?status=]` | both library views |
| `GET`/`PATCH /v1/games/{id}` | `GameDetailDialog`, `ui/GameEditForm` |
| `DELETE /v1/games/{id}` | `DeleteGameDialog`, including `delete_files`/`delete_prefix` |
| `GET`/`PATCH /v1/games/{id}/config` | `OverridesEditor` |
| `POST /v1/games/{id}/launch`, `/stop` | Play/Stop, tile double-click, context menu |
| `GET /v1/games/{id}/artwork` | `ui/ArtworkStore` — grid tiles and `ui/GameEditForm`'s hero/cover box |
| `GET /v1/games/{id}/artwork?type=hero` | `ui/GameEditForm`'s banner, in place of the cover when a game has one |
| `GET /v1/games/{id}/metadata` | `ui/HoverCard` (ProtonDB tier, developer, genres) on hover; `GameDetailPageDialog` (context menu → *More details…*) for screenshots, trailers, requirements, DLC, content descriptors, achievements; also `art_candidates.cover`/`.hero` for `ArtworkPickerDialog` |
| `POST /v1/games/{id}/metadata/refresh` | the tile context menu's *Refresh metadata && cover art*, and *Library → Fetch missing cover art* |
| `POST /v1/games/{id}/artwork?type=` | *Choose cover art…* / *Choose hero art…* (`ArtworkPickerDialog`) |
| `GET /v1/games/{id}/metadata/matches`, `POST .../metadata/match` | `ArtworkPickerDialog`'s SteamGridDB game row: which game the art comes from, with a search for another name |
| `POST /v1/games/{id}/run` | *Run in prefix…* (`RunInPrefixDialog`) |
| `POST /v1/games/{id}/finish-install` | *Mark as installed* |
| `GET /v1/games/{id}/installer`, `POST .../install` | *Install…* (`InstallGameDialog`), for a `needs_install` or `broken` game |
| `GET /v1/games/{id}/install/progress` | `ui/DownloadTracker`, for the tile's and the downloads panel's "Installing… 1.2 GB" while `game.install.*` says one runs |
| `GET`/`POST /v1/library/artwork` | `SourcePage`'s Not installed covers, through `ui/ArtworkStore::TitleCover` |
| `POST /v1/games/{id}/relocate`, `/v1/library/relocate` | *Move to Mira's folders…* (tile and batch menus), *Move games into Mira's folders…* (sidebar) |
| `POST /v1/library/scan` | on startup, and *View → Refresh library* |
| `GET /v1/library`, `POST /v1/library/install\|update` | `SourcePage`'s Not installed tiles, and Update on its library tiles |
| `/v1/{epic,gog,itch,amazon,humble}/*` | `SourcePage` for each store |
| `/v1/launchers/*` | `SourcePage` for Battle.net, Ubisoft Connect, the EA app |
| `GET`/`PATCH /v1/config`, `/reset` | `SettingsDialog`; `RunnersPage`'s *Make default* and *Pick automatically instead* (`default_runner.windows`) |
| `GET /v1/config/schema` | generates `SettingsDialog` and `OverridesEditor` |
| `GET /v1/runners` | runner pickers, and `RunnersPage`'s Installed card |
| `GET /v1/runners/catalog` | `RunnersPage`'s Get more card |
| `POST /v1/runners/download` | `RunnersPage`'s Install; progress through `ui/DownloadTracker` |
| `GET /v1/runners/sources` | `RunnersPage`'s source picker on Get more |
| `GET /v1/runners/updates`, `POST /v1/runners/update` | `RunnersPage`'s *Update to …* on installed builds, then an offer to remove the old build |
| `GET /v1/runners/tools`, `POST /v1/runners/tools/{id}/setup` | `RunnersPage`'s banner when umu-launcher or winetricks is missing |
| `POST /v1/steam/scan` | *Library → Import Steam library* |
| `POST /v1/lutris/import` | *Library → Import Lutris games* |
| `GET /v1/events` | `EventStream` |

Events handled: `game.added` (and its `open_config`), `game.updated`,
`game.removed`, `game.state`, `game.launched`,
`game.metadata_ready`/`.metadata_failed`, `game.install.*`,
`runners.download.started`/`.finished`/`.failed`, the store/launcher setup,
`library.install.*` and `humble.download.*` events (downloads panel and
source pages), and `library.artwork_*` on a source page.

**Dispatch on the event type, always.** Only `game.added` and `game.updated`
carry a game record, and the library views check for exactly those two
rather than treating whatever is left over as a game. They did not, once:
a `runners.download.started` payload is a JSON object, so it parsed into a
game with every field empty and the library grew a blank tile on every
runner download. `ParseGameSummary` now also requires a non-empty string
`id`, as a second line of defence — a `tricks.*` payload does carry one, and
would have blanked a real row rather than adding a fake one.

Two notes on shapes that are easy to get wrong:

- `game.state` carries only `id`/`state` and a few launch-specific fields —
  not `play_seconds` or `last_played_at`. An `exited` is a signal to
  re-fetch, not something to patch a row from.
- **A Steam-launched game never emits `game.state` at all.** Under the
  default `steam.launch_mode: "steam"`, mirad hands the game to
  `steam://rungameid/<appid>` and never spawns it, so `POST .../launch`
  answers `{"status": "launched_via_steam"}` and publishes `game.launched`
  instead. Marking such a game as running pins it under "Playing now"
  forever, because nothing will ever say it stopped — hence
  `LaunchResult::tracked`, and hence `game.launched` clearing the id rather
  than being ignored.
- `GET /v1/runners` used to report the same build once per search path it
  was found under, which a symlinked Steam directory makes the normal case.
  The frontend collapsed those itself for a while; it no longer does, and
  should not — `runner::DeduplicateBuilds` handles it in the daemon, where
  every other API client gets the fix too. See `docs/api.md`.

## Telling the user things

`ui/Notify` owns every message, so two screens cannot disagree about what a
failure looks like. Anything that isn't a question is a desktop
notification, in one of two kinds:

| kind | when | how |
|---|---|---|
| **persistent** | something went wrong — `notify::Failed`, `FailedWithHint`, `FailedWithAction`, `Warn` | critical urgency, `expire_timeout` 0: stays until dismissed |
| **transient** | a small confirmation of something with no visible result of its own — `notify::Notice` | normal urgency, `expire_timeout` -1: the desktop's own default |

Only two things are still popups: a question the caller can't proceed
without (`Confirm`, `ConfirmUnsaved`), and content that *is* the answer to a
button (`Info`). A dialog that already has
a status line of its own (`RunnersPage`, `ArtworkPickerDialog`, the log
viewer's text pane) reports its errors there instead of notifying over
itself.

**Success whose result is already on screen gets nothing.** A scan or import
that adds games, a batch delete, a cover arriving, a metadata refresh — the
grid changing is the feedback. A notice only goes out when nothing in Mira's
window would change otherwise ("No new Steam games found.", "Added to the
application menu."). Notifications that just narrate work starting
("Fetching metadata…") don't exist at all.

The route is `org.freedesktop.Notifications` over the session bus
(`ui/SystemNotifier`) — the cross-desktop standard KDE, GNOME, XFCE,
Cinnamon, dunst, mako and swaync all implement, so nothing here is specific
to one desktop. A `desktop-entry` hint of `mira` lets the shell show Mira's
own name and icon and list it in per-application notification settings
(where the user controls how long things stay up — Mira has no setting of
its own for that). `FailedWithAction` adds a button (and a click on the
notification itself) routed back through the `ActionInvoked` signal, raising
Mira's window first.

With no notification service on the bus at all (a bare window manager), a
failure falls back to a modal popup and a `Warn`/`Notice` to a card stacked
bottom-right of the window — only so the message isn't lost. Cards dismiss
on click; a transient one also after six seconds.

Every popup and card sets `Qt::PlainText`. mirad's error messages quote
paths and command fragments, and rich text would silently eat anything that
looked like a tag. Failures show mirad's own message verbatim under a
sentence naming what failed — the daemon explains its refusals better than a
rewrite would.

## Cover art

Real artwork comes from `GET /v1/games/{id}/artwork`, which serves whatever
image the metadata fetcher cached (Steam's CDN for a Steam-owned game,
SteamGridDB otherwise — see `docs/api.md`). `ui/ArtworkStore` owns the whole
story and hands out a pixmap that is never empty:

- **404 is the normal answer**, not an error. Most games have no artwork
  cached, and one message per game on a fresh library would be unusable.
  `ui/CoverArt` generates the placeholder for those, from a hue derived from
  the game's **id** — stable across restarts, renames and machines, so a
  tile can be learned by sight.
- **Ask once per game.** An id that has answered either way is not asked
  again until `game.metadata_ready` or an explicit refresh invalidates it.
  Without that rule an empty library becomes a request loop, because every
  repaint asks for a cover.
- **Four requests in flight, maximum.** Each one is a thread and a socket
  (`client/Async.h`), and artwork is per game, so a 500-game library would
  otherwise open 500 of each the moment the window appears.
- **Keep the original, scale on demand.** The zoom slider changes the tile
  size continuously; re-decoding a JPEG per step would be visible and
  re-fetching it absurd. A rename drops the rendered copies (the
  placeholder's initials changed) but not the fetched image.

The grid and `ui/GameEditForm`'s hero/cover box share one store, so a cover
is fetched, decoded and cached once for both.

**No key, no art, and now it says so.** A non-Steam game has no free cover
source other than SteamGridDB, so with `steamgriddb.api_key` unset mirad
fails the fetch with `no_steamgriddb_key` instead of quietly succeeding at
nothing (`docs/api.md`). The frontend matches on that **code**, never on the
message, and raises it once per session however many games report it: a
popup offering to open Settings when the user asked for the fetch, a toast
when a background scan did. Every other metadata failure is reported only
for a game the user asked about.

**Re-fetching.** mirad fetches metadata only when a game is *first*
detected, so a game whose fetch failed — or any non-Steam game from before
`steamgriddb.api_key` was set — keeps its placeholder until something asks
again. Two ways to ask: a tile's right-click *Refresh metadata && cover
art*, and *Library → Fetch missing cover art*, which does it for every game
the store has no image for. The bulk path raises one toast for the batch
rather than one per game.

There is no `has_artwork` on a game summary, so "does this game have
artwork" can only be answered by asking for it. That is the reason for the
ask-once and in-flight rules above; a flag on `GET /v1/games` would remove
the need for both.

**`ArtworkPickerDialog`** (*Choose cover art…* / *Choose hero art…*, one
instance per slot) lists a SteamGridDB game's cached `art_candidates.cover`
or `.hero` by style and dimensions rather than showing a thumbnail grid: each
candidate's own `url`/`thumb` point at SteamGridDB's CDN, and the frontend
has no HTTP client for the open internet, only mirad's socket. The row
matching the slot's `candidate_id` (GET .../metadata) is marked "(current)"
and pre-selected on open. Picking a row applies it immediately (`POST
.../artwork?type=`) and, once its own `EventStream` sees
`game.artwork_selected`, redraws the preview from the real image mirad just
fetched and cached — that redraw *is* the preview. The same event also
reaches `LibraryWindow::HandleGameEvent`, which invalidates the game's
`ArtworkStore` entry (cover) or `GameEditForm`'s own banner cache (hero) so
the grid tile/edit page follow without waiting for the picker to close. A game
with no candidates cached (no `steamgriddb.api_key` set, or SteamGridDB has
no match for the name) says so instead of showing an empty list — this now
includes Steam-owned games too, which get SteamGridDB candidates as
alternates alongside their Steam-CDN default when a key is set.

## Tests

`mira_gui_tests` (`frontend/tests/`) is its own executable, not more files
in the root's `tests/`. That target links `mira_core`, and a frontend test
able to reach backend code could pass against an implementation the real
`mira-gui` never talks to. This one links only Qt6::Core and the
header-only third-party deps the client itself uses.

It covers the layers with no event loop and no socket in them: the JSON
mapping, the SSE payload parsers, the library sort order, and the pure
client-side rules (runner dedupe, dotted-key expansion, the display-string
round trip).
Widgets are exercised against a real daemon instead — run one on its own
socket and point the frontend at it:

```sh
XDG_CONFIG_HOME=~/.mira-dev mirad --socket ~/.mira-dev/mirad.sock &
MIRA_SOCKET=~/.mira-dev/mirad.sock build/frontend/mira-gui
```

`$MIRA_SOCKET` is the same override `mirad --socket` takes. Without it a
daemon started without that `XDG_CONFIG_HOME` binds the default socket path
and silently takes over the one a sandboxed daemon was already serving,
since `XDG_CONFIG_HOME` separates config but never the socket. The window
footer shows the socket actually in use, so which daemon you are talking to
is visible rather than assumed.

## Not built yet

Nothing outstanding right now: winetricks (`WinetricksDialog`), runner
delete (`RunnersPage`), the wider game detail page
(`GameDetailPageDialog`), the per-game log viewer (`LogViewerDialog`), a
GameMode status indicator (Settings, Launching category), manual single-game
add (`AddManualGameDialog`), the desktop-entries import picker
(`DesktopEntryImportDialog` — this is now how a Flatpak app gets added, since
the Flatpak runner itself was removed backend-side) and the `delete_metadata`
remove-game option are all in.
