# The frontend (`mira-gui`)

What the Qt frontend does, how it is put together, and which parts of
[`api.md`](api.md) it uses. For *why* the frontend is a REST client rather
than a second copy of the backend, see [`architecture.md`](architecture.md);
that decision is load-bearing and is not restated here.

## Two views, both kept

`mira-gui` opens on **the grid** (`views/LibraryWindow`): cover tiles, a
filter sidebar, a search box, a details panel. It is modelled on Playnite's
shelf, which is the interaction most people arriving at a Linux game
launcher already know.

**The table** (`views/MainWindow`) shows every field of every game at once.
It is reachable as `mira-gui --classic`, or from the grid's *View → Open
classic table view*, which opens it as a second top-level window rather than
swapping the grid out.

Neither is a fallback for the other. The grid is the better browser; the
table is the better audit tool for a library that was just scanned, where
the question is "what did detection get wrong" and the answer is a column.
Both are thin clients over the same `MiradClient` calls and each holds its
own `EventStream`, so two open windows cannot disagree about what the daemon
said.

### Selection model

One click selects a tile and fills the details panel. A second (double)
click launches. Right-click opens the per-game menu. Launching on the first
click would turn a misclick into a started game, so a single click never
launches anything.

### Keyboard

`Ctrl+Q` quits, `Ctrl+W` closes one window, `F1` lists every key the focused
window responds to. Those three are installed by `ui/Shortcuts` on both
views, so the classic table — which has no menu bar to hang an action on —
still has them.

Quit goes through `QApplication::closeAllWindows()` rather than `quit()`,
because `LibraryWindow` saves its layout in `closeEvent` and a quit that
skipped that handler would drop the prefs silently.

The grid adds: `Ctrl+F` search, `Esc` (clears the search first, the
selection second), `Ctrl+1`–`Ctrl+8` sidebar filters, `F5`/`Ctrl+R` refresh,
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
  `src/config/Schema.cpp`, and `SettingsDialog` is generated entirely from
  `GET /v1/config/schema` — almost no setting name is hardcoded in the
  frontend. Adding a backend setting requires no frontend change.
- **`frontend.toml`** holds the frontend's own state and preferences:
  window size, tile size, which sidebar filter and sort were selected,
  splitter widths, and whether to scan the library on startup. The backend
  stores it verbatim and never validates it, reachable as the opaque
  `frontend` key of `GET`/`PATCH /v1/config` (see `FrontendPrefs`).

  | key | what it does |
  |---|---|
  | `window_width`, `window_height` | remembered window size |
  | `sidebar_width`, `details_width` | remembered splitter layout |
  | `tile_width` | cover tile size (clamped to the zoom slider's range) |
  | `library_filter` | which sidebar filter was selected |
  | `sort_by`, `sort_descending` | grid order — see `ui/LibrarySort` |
  | `scan_on_startup` | whether opening the frontend runs `POST /v1/library/scan` |
  | `notifications` | `auto` / `system` / `in_app` — see "Telling the user things" |

  `scan_on_startup` and `notifications` get rows in the settings screen, in
  an "Interface (this frontend only)" group above the schema-driven ones.
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
| `GET`/`PATCH /v1/games/{id}` | `GameDetailDialog` |
| `DELETE /v1/games/{id}` | `DeleteGameDialog`, including `delete_files`/`delete_prefix` |
| `GET`/`PATCH /v1/games/{id}/config` | `OverridesEditor` |
| `POST /v1/games/{id}/launch`, `/stop` | Play/Stop, tile double-click, context menu |
| `GET /v1/games/{id}/artwork` | `ui/ArtworkStore` — grid tiles and the details panel |
| `POST /v1/games/{id}/metadata/refresh` | the details panel's button, the tile context menu, and *Library → Fetch missing cover art* |
| `POST /v1/games/{id}/run` | *Run in prefix…* (`RunInPrefixDialog`) |
| `POST /v1/games/{id}/finish-install` | *Mark as installed* |
| `POST /v1/library/scan` | on startup, and *View → Refresh library* |
| `GET`/`PATCH /v1/config`, `/reset` | `SettingsDialog` |
| `GET /v1/config/schema` | generates `SettingsDialog` and `OverridesEditor` |
| `GET /v1/runners` | runner pickers, and `RunnerDialog`'s installed list |
| `GET /v1/runners/catalog` | `RunnerDialog`'s available list |
| `POST /v1/runners/download` | `RunnerDialog`'s Download |
| `POST /v1/steam/scan` | *Library → Import Steam library* |
| `GET /v1/events` | `EventStream` |

Events handled: `game.added`, `game.updated`, `game.removed`, `game.state`,
`game.launched`, `game.metadata_ready`/`.metadata_failed`,
`runners.download.started`/`.finished`/`.failed`.

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

Three shapes, and `ui/Notify` owns the first two so that two screens cannot
disagree about what a failure looks like:

| shape | when | where |
|---|---|---|
| **popup** | what you just asked for did not happen, or Mira needs an answer first | `notify::Failed`, `FailedWithHint`, `Info`, `Confirm` |
| **toast** | something finished on the daemon's schedule, not yours | `notify::Toast` |
| **inline status** | a dialog reporting on an operation it owns and can say "still going" about | the dialog's own label — see `RunnerDialog` |

The split follows the API. Anything that returns **202** (`/runners/download`,
`/games/{id}/metadata/refresh`, `/games/{id}/tricks`) finishes minutes later
and reports through an SSE event, by which time the user has moved on — a
modal for that is an ambush. Anything answering the request in front of you
gets a popup.

A toast goes to one of two places, and `notifications` in `frontend.toml`
decides which:

| value | behaviour |
|---|---|
| `auto` (default) | the desktop's notification service when Mira's window is not the active one, an in-window card when it is |
| `system` | always the desktop's service |
| `in_app` | always the in-window card |

`auto` is the one that matters. A background job finishing while you are
looking at something else is exactly what the desktop's notification area
is for — it survives Mira being minimised, lands in the shell's history
(Plasma's, on KDE) and obeys Do Not Disturb. A system popup for something
that just happened in the window under your cursor is noise the desktop
then keeps a record of.

The system route is `org.freedesktop.Notifications` over the session bus
(`ui/SystemNotifier`), with a `desktop-entry` hint of `mira` so the shell
shows Mira's own name and icon and lists it in per-application notification
settings. `QGuiApplication::setDesktopFileName("mira")` backs that up for
the compositor. Urgency maps to the level: an error is `critical`, which
most shells do not dismiss on a timeout.

Any failure falls back to the in-window card, so choosing `system` on a
desktop with no notification service loses nothing.

In-window cards stack bottom-right, dismiss themselves (longer for an error
than for a success), and dismiss on click. They attach to the top-level
window rather than to the widget that raised them, so a card survives the
dialog that started the work.

Every popup goes through one helper that sets `Qt::PlainText`. mirad's error
messages quote paths and command fragments, and rich text would silently eat
anything that looked like a tag.

Failure popups show mirad's own message verbatim under a sentence naming
what failed. The daemon explains its refusals better than a rewrite would.

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

The grid and the details panel share one store, so a cover is fetched,
decoded and cached once for both.

**Re-fetching.** mirad fetches metadata only when a game is *first*
detected, so a game whose fetch failed — or any non-Steam game from before
`steamgriddb.api_key` was set — keeps its placeholder until something asks
again. Three ways to ask: the details panel's *Refresh cover art &
metadata* button, the same entry on a tile's right-click menu, and
*Library → Fetch missing cover art*, which does it for every game the store
has no image for. The bulk path raises one toast for the batch rather than
one per game.

There is no `has_artwork` on a game summary, so "does this game have
artwork" can only be answered by asking for it. That is the reason for the
ask-once and in-flight rules above; a flag on `GET /v1/games` would remove
the need for both.

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

Everything here exists in the API and has no path through the UI:

- **`GET /v1/games/{id}/metadata`** — the store info behind the artwork:
  description, genres, categories, release date, developers/publishers,
  price, metacritic, a Steam review summary, and a ProtonDB tier. Only the
  cover image is used today. The ProtonDB tier in particular belongs on a
  Linux launcher's game page.
- **`POST /v1/games/{id}/tricks`** — winetricks verbs. No catalog endpoint
  exists, so this wants a free-text verb field (plus, perhaps, a short list
  of common ones) shaped like `RunInPrefixDialog`, and it 404s for a native
  game or a prefix that was never provisioned, so the menu entry should be
  disabled with the reason rather than hidden.
- **`tricks.started`/`.finished`/`.failed`** — unhandled. They are the only
  report a verb ever makes, since the endpoint returns 202.

- `DaemonSupervisor` — starting and supervising `mirad` from the frontend.
  `architecture.md` describes the design; today `mira-gui` assumes the
  daemon is already running and reports it unreachable if not.
- A tray icon, and the "keep running in background" mode that goes with it.
