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

  Only `scan_on_startup` gets a row in the settings screen, in an
  "Interface (this frontend only)" group above the schema-driven ones. The
  rest are implicit UI state: they are saved by using the window, not by
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
`runners.download.started`/`.finished`/`.failed`.

Two notes on shapes that are easy to get wrong:

- `game.state` carries only `id`/`state` and a few launch-specific fields —
  not `play_seconds` or `last_played_at`. An `exited` is a signal to
  re-fetch, not something to patch a row from.
- `GET /v1/runners` reports the same build once per search path it is found
  under, and on a typical Arch/Steam setup `~/.steam/steam` is a symlink to
  `~/.local/share/Steam`. Two entries sharing a `reference` are the same
  runner by definition, so the client collapses them
  (`DedupeRunnersByReference`) rather than offering a choice that isn't one.

## Cover art

There is none from the backend yet. `ui/CoverArt` generates a placeholder
from a hue derived from the game's **id** — stable across restarts, renames
and machines, so a tile can be learned by sight. When real artwork lands
this becomes the fallback for games that have none.

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

- Real cover art (waiting on the backend).
- `DaemonSupervisor` — starting and supervising `mirad` from the frontend.
  `architecture.md` describes the design; today `mira-gui` assumes the
  daemon is already running and reports it unreachable if not.
- A tray icon, and the "keep running in background" mode that goes with it.
