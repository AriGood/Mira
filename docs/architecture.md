# Architecture

## What this is

Mira is a Linux game launcher: native games plus Windows games via Wine/Proton.
It is three programs sharing one contract:

| Program    | What it is                                    | Role |
|------------|------------------------------------------------|------|
| `mirad`    | the backend daemon                             | owns all state, does all the work |
| `mira`     | a command-line client                          | a REST client over the same API the GUI uses |
| `mira-gui` | the Qt frontend                                 | a REST client over the same API the CLI uses |

**The only thing that connects them is the REST API described in
[`api.md`](api.md), served over a Unix domain socket.** This is the single
most load-bearing design decision in the project, so it is worth stating
plainly: `mira-gui` does not link against `mira_core`, does not read
`settings.toml` or `games.toml` directly, and does not know how detection or
provisioning work. It sends HTTP requests to `mirad` and renders what comes
back, exactly as `mira` does. If a change to the frontend ever requires
reaching past the API to get something done, that is a sign the API is
missing an endpoint — the fix is to add one, not to grant the frontend a
back door.

Two consequences fall out of this:

- **`mira` (the CLI) is the frontend's canary.** It hits the same socket the
  same way. Anything awkward, slow, or impossible from the CLI will be
  exactly as awkward from the GUI, so CLI commands are added alongside each
  API endpoint as a cheap way of proving the endpoint is actually usable.
- **The backend can be developed, tested, and used to completion without the
  frontend existing at all.** Everything in this repository right now (the
  library, the settings, the games) is reachable and useful through `mira`
  alone.

## Where the code lives

```
src/
  core/     free-standing helpers with no dependency on the rest of the app:
            Result<T> (error handling with no exceptions across interfaces),
            Command (the argv+env+cwd every runner produces), Paths (XDG
            resolution), Strings, Log, and the TOML<->JSON bridge.
  config/   Schema (every setting declared once), Config (loads/saves
            settings.toml, plus the sibling frontend.toml the frontend's
            own opaque settings live in), Resolver (layered
            default -> file -> per-game lookup with provenance),
            KnownExePatterns / RunnerSources (just data: the default
            installer/helper-executable name lists and the Proton-GE/
            Wine-GE download sources — edit either directly, no schema
            knowledge needed; both feed schema defaults that stay
            user-overridable).
  model/    Game, RunnerBuild, Event, Candidate — plain structs plus
            ToJson/FromJson. No behaviour lives here.
  store/    GameStore — the games.toml-backed source of truth for the
            library.
  library/  Detector (scores executables in one folder), Scanner (walks
            library roots, reconciles against GameStore, retries
            provisioning for anything still setting_up), AutoSetup (turns
            a detection into a stored game), Watcher (inotify, drives
            Scanner automatically; also debounces and auto-extracts a
            dropped archive when scan.auto_extract_archives is on),
            ArchiveExtractor (the zip/rar/tar*/7z extraction itself),
            WinePrefix (shared prefix-directory detection, used by both
            Detector and Scanner so neither re-discovers the other's
            prefixes as games).
  runner/   IRunner + NativeRunner/ProtonRunner/WineRunner/SteamRunner,
            RunnerRegistry (resolves "kind:name" -> a concrete runner +
            build), Exec (RunAndWait for provisioning/downloads,
            SpawnDetached for an actual game launch), Downloader (lists/
            installs Proton-GE/Wine-GE builds from GitHub releases).
  proc/     ProcessSupervisor — supervises a launched game: crash vs.
            clean-exit classification, playtime checkpointing,
            SIGTERM->SIGKILL stop escalation.
  desktop/  DesktopEntries — generates/syncs .desktop menu entries for
            ready games, always launching back through Mira so playtime
            is never bypassed by a menu launch.
  steam/    Vdf (a from-scratch parser for Valve's KeyValues/VDF text
            format), SteamDetector (reads Steam's own files directly:
            library folders, installed apps, which Proton build/prefix an
            app uses), SteamScanner (detect -> upsert into GameStore, the
            Steam equivalent of library::Scanner).
  api/      EventBus (in-memory pub/sub) and Server (the REST routes) —
            see api.md for the surface this registers.
  cli/      main.cpp for `mira` — see cli.md for what it does.
  mirad_main.cpp   entry point for the daemon.

frontend/   the Qt frontend (mira-gui), in four layers that only ever
            depend downward:
  client/   everything that talks to mirad and nothing that draws:
            Transport (one socket round trip: timeouts, the error
            envelope), JsonMapping (JSON <-> the structs in Types.h),
            Async (the worker-thread hop and the liveness rule that
            makes it safe), MiradClient (one method per endpoint),
            EventStream (the SSE connection).
  ui/       presentation shared by more than one view or dialog:
            GamePresentation (status/confidence colours and the
            date/playtime formatters), CoverArt (generated placeholder
            covers), GameTileDelegate, GameDetailsPanel, GameActions
            (launch/stop/delete with their prompts).
  views/    LibraryWindow (the default cover-art grid) and MainWindow
            (the classic table, kept as --classic).
  dialogs/  GameDetailDialog + OverridesEditor (per-game config),
            SettingsDialog + SettingsCategories (generated from the
            config schema), DeleteGameDialog.
            A separate CMake project scope; never add a
            target_link_libraries(mira-gui PRIVATE mira_core).

tests/      doctest-based unit tests, one executable (mira_tests).
packaging/  the systemd user unit and the .desktop entry.
```

Routes are registered in `src/api/Server.cpp` (`Server::RegisterRoutes`);
that function is the API's actual implementation, and `api.md` is its
human-readable mirror — when they disagree, the code is correct and the doc
is stale (fix the doc).

## Storage: TOML, not a database

Everything a user might want to inspect, hand-edit, back up, or put under
version control lives in **one directory**, `$XDG_CONFIG_HOME/mira` (normally
`~/.config/mira`):

```
~/.config/mira/
  settings.toml   backend settings + a passthrough [frontend] table
  games.toml      the whole library, one [[game]] per entry
```

This was a deliberate choice over SQLite. At the scale this targets (a
single user, hundreds of games) a database buys query performance that
nothing here needs, at the cost of the file being opaque to the person who
owns it. TOML gives up nothing that matters here and gains a config a user
can read, diff, edit by hand, and drop into a dotfiles repo. `Config` and
`GameStore` both hold their data in memory behind a mutex and write the
whole file back on every mutation — simple, and fast enough at this scale.

The **socket** is the one piece of daemon state that is *not* in that
directory: `$XDG_RUNTIME_DIR/mira/mirad.sock` is transient IPC, recreated
every run, and actively wrong to back up (a stale socket path is worse than
a missing one).

`settings.toml`'s `[frontend]` table is opaque to the backend by design —
`config/Schema.cpp` only declares and validates backend keys. The frontend
can store whatever it wants there (window geometry, theme, last-selected
tab) via `PATCH /v1/config` with a `frontend` key, and the backend stores and
returns it verbatim. This is what lets "one settings file" work without the
backend needing to track the frontend's schema.

## Running the daemon: three supported paths

`mirad` never daemonizes itself (no double-fork, no self-backgrounding) — it
runs in the foreground and expects something else to manage that. There are
three legitimate ways to run it, and the project explicitly supports all
three rather than picking one:

### 1. systemd user service (persistent, backend-managed)

```
systemctl --user enable --now mirad.service
```

`packaging/mirad.service` is a normal `Type=simple` user unit with
`Restart=on-failure`. Once enabled, `mirad` starts at login and keeps running
independent of whether any frontend is open — the library is watched and
games stay provisioned even if `mira-gui` is never launched. This is the
right choice for someone who wants "drop a game in the folder, it's ready"
to work with no window open at all, and who doesn't mind one small resident
process (see the idle-cost notes below for why that process costs close to
nothing when idle).

`journalctl --user -u mirad` is the log; `systemctl --user status mirad`
is the health check. Stopping it is `systemctl --user stop mirad` — the
socket disappears and `mira`/`mira-gui` report the daemon unreachable until
it's started again, exactly as designed (a game already provisioned stays
provisioned; only new detection/provisioning pauses).

### 2. Frontend-managed (no persistent process by choice)

For someone who would rather not have *any* long-lived background process:
`mira-gui`, on launch, checks whether `mirad` is already reachable at the
configured socket. If not, it spawns `mirad` itself as a child process (not
via systemd) and supervises it — the daemon's lifetime becomes tied to the
frontend's. Closing the frontend can either stop the daemon immediately or
(if the user enables "keep running in background" / tray mode) leave it
running until the frontend is told to fully quit.

This is currently a documented design, not yet implemented — `mira-gui`
today assumes `mirad` is already running and just reports it unreachable if
not (see "Built: the frontend" below for what does exist). Recorded here
because it is a load-bearing decision for how the frontend is built, not an
afterthought: whoever implements this should add a small `DaemonSupervisor`
in the frontend that:

1. Probes `GET /v1/health` on the configured socket.
2. On failure, resolves `mirad` next to its own binary (mirroring what
   `mira daemon` already does in `src/cli/main.cpp`) or on `PATH`, and
   spawns it with `QProcess`, capturing its stderr for a log view.
3. Tracks whether *it* started the daemon (vs. finding one already running
   via systemd) — only a self-started daemon should ever be torn down on
   exit; a systemd-managed one is left alone regardless of frontend state.
4. On the frontend's own exit (or tray-quit), stops a self-started daemon
   with the same SIGTERM path `systemd` would use.

### 3. One-shot, no persistent anything

For someone who wants to use Mira purely to set a library up once and never
run it again: run `mirad` from a terminal, do the setup work
through `mira-gui` or `mira`, then stop it (Ctrl-C, or `mira` has no stop
command for a foreground-launched daemon by design — that's the terminal's
job). No systemd unit is required to exist or be enabled for this to work;
nothing about the daemon assumes it is managed by systemd. A `mirad
--scan-once` mode (scan configured roots, auto-provision, print a summary,
exit) is planned for this persona specifically, so it doesn't require
leaving a process attached to a terminal at all — see the open items below.

### Why not just pick one?

Because the two audiences want opposite things and neither is wrong: some
people want zero background processes ever (path 2 or 3); some people want
the library staying current even with every window closed (path 1). Making
both first-class costs one health-check-and-spawn code path in the frontend
and one static unit file — small, compared to forcing a workflow on people
who explicitly don't want it.

## Application menu integration

`packaging/mira.desktop` installs a normal desktop entry (`Exec=mira-gui`)
so "Mira" appears in the application menu after installation, independent of
which of the three paths above is in use. Launching it always starts the
frontend; the frontend is what decides (per path 1/2/3 above) whether it
needs to also start the daemon. There is no separate desktop entry for
`mirad` — a background daemon is not a thing a user "launches" from a menu.

Separately, `desktop::DesktopEntries` (`src/desktop/DesktopEntries.cpp`)
gives *each ready game* its own menu entry (`mira-<id>.desktop`), synced
after every scan and every change to a game or to `desktop_entries.*`
settings. Its `Exec=` line is always `mira launch <id>` (or the frontend,
if `desktop_entries.exec_mode` is `"frontend"`) — never the game's own
executable directly, no matter how tempting that shortcut looks, because
that's exactly what would make a menu launch invisible to
`proc::ProcessSupervisor`'s playtime/crash tracking. Only ever creates or
removes files it created itself (`mira-<id>.desktop`) in the configured
directory.

## Idle cost

The daemon is meant to be safe to leave running via path 1. That only holds
if idle really means idle:

- `EventBus::WaitNext` (`src/api/EventBus.cpp`) is a condition-variable wait,
  not a poll. SSE connections block a thread each; they cost nothing on the
  CPU until an event is published. The 20-second wait ceiling inside it
  exists solely to detect a vanished client on an *open* connection — it is
  not a daemon-idle wakeup, and it only runs while something is connected.
- `mirad_main.cpp`'s shutdown handling is `sigwait`, not a signal-handler
  flag polled in a loop.
- `library::Watcher` (`src/library/Watcher.cpp`) blocks in `epoll_wait` with
  no timeout over inotify + an eventfd (for `Stop()`) + a timerfd used only
  for debounce. That timerfd is the one deliberate exception, and it proves
  the rule rather than breaking it: it is armed *only* while a newly-created
  directory is being watched for size stability (polling its total size
  every 500ms to tell "still copying" from "done"), and disarmed the moment
  nothing is pending. At rest, with no directory mid-copy, it costs nothing.
  One inotify watch per library root, non-recursive, so watch count is
  O(roots) rather than O(library size) — recursive watching of a large
  library is the usual way this kind of daemon gets expensive.

## Replaceability

Two seams are treated as real interfaces because they are known to need a
second implementation later; everything else stays concrete until it
doesn't:

- **Runners** (`runner/IRunner.h`) — `umu-run` is a means to a working
  Proton launch, not a permanent dependency. Validated, not just asserted:
  `NativeRunner`, `ProtonRunner`, and `WineRunner` (plain system Wine, no
  Proton) are three real implementations of the same four-method interface,
  and CI runs `grep -rniE 'umu|protonpath|gameid|steam_compat' src/
  --exclude-dir=runner` — every umu/Proton-specific token is still contained
  to `runner/ProtonRunner.{h,cpp}` (a couple of explanatory comments elsewhere
  just name the tool; none encode its env vars or behavior). A future custom
  Proton runner replacing umu is a fourth file, not a redesign.
- **Detection rules** — per-user heuristics that are certain to need
  retuning; `config/Schema.cpp`'s `detect.*` keys already externalize the
  weights this will use.

## Built: library detection, scanning, and watching

`library/Detector.{h,cpp}` scores the executables inside one already-found
folder using the `detect.*` schema keys (`DetectorSettings` mirrors those
keys as plain values, so it's testable with no config file involved).
Deliberately not a polymorphic rule-chain: each named pass in `detect.rules`
(`deny_patterns`, `name_similarity`, `depth`, `shallowest`) is a plain
function applied in the configured order, skipped if its name is absent from
the list. That gives the documented promise — "removing a rule disables
it" — without an interface that has no second implementation to justify it
(see Replaceability above on why that restraint matters).

Two things beyond straight scoring, both found by testing against real
game folders rather than by review: a candidate can be flagged
`is_installer` (`detect.installer_name_patterns` name match, plus a size
signal — either the file itself is large, or it sits beside one that is,
since a common real packaging shape is a small stub `.exe` next to a
much larger separate payload); a game whose only/best candidate is one is
stored `needs_install` rather than silently treated as launchable. The
default `deny`/`installer` name patterns live in one small, dedicated file,
`src/config/KnownExePatterns.h` — not buried in `Schema.cpp`'s
registration code — specifically so extending the list (a contributor's
common task, since it's inherently an ever-growing list of known engine
and launcher helper executables) needs no schema knowledge, just editing
an obvious array. Grounded in real prior art where checked: `*crashhandler*`
exists because Unity's `UnityCrashHandler*.exe` getting auto-picked over
the real game exe is a documented, still-open Lutris bug
(github.com/lutris/lutris/issues/6881).

`library/Scanner.{h,cpp}` walks each enabled library root one level deep —
every immediate subdirectory is one game, matching "drop a folder in and
it's picked up". A directory already known by `install_path` is never
re-detected, so a scan can never clobber a correction made through `PATCH`.
It excludes `prefix_root` and anything that looks like a Wine prefix
(`system.reg` + `drive_c`, or `pfx/`) before ever calling the detector — this
is the regression the whole design has to guard against, since the default
prefix location sits *inside* the library root by default, and it's covered
by an end-to-end test (`tests/detector_scanner_test.cpp`,
`tests/watcher_test.cpp`) rather than left to code review.

`library/AutoSetup.{h,cpp}` turns one `Detector::Result` into a stored
`model::Game` and publishes `game.added` — before any provisioning happens,
so the frontend can raise its config menu immediately rather than waiting on
work that hasn't started. A native game needs no provisioning and is stored
`ready` right away; a Windows game is stored `setting_up` and simply waits
there — there is no runner layer yet to provision its prefix.

`library/Watcher.{h,cpp}` is what makes detection automatic rather than
requiring `mira scan`/`POST /v1/library/scan` by hand: one inotify watch per
enabled root (read from `library_roots` once at startup — adding or removing
a root needs a restart to be watched, though `POST /v1/library/scan` always
picks up the current list immediately), debounced by polling a
newly-created directory's total size until it stops changing for
`scan.debounce_ms`, then running the same `Scanner::ScanRoot` logic the
manual endpoint uses. A deletion needs no debounce and rescans its root
immediately, so a removed game is marked `missing` promptly.

## Built: runners

`runner/IRunner.h` is four methods: `kind()`, `Discover(config)` (installed
builds — empty for a runner with no such concept), `Provision(game, build)`,
`BuildCommand(game, build)`. No `capabilities()`/`settings_schema()` yet —
nothing consumes them until `GET /v1/runners/{kind}/schema` exists, and
speculative interface surface with no caller is exactly the kind of
abstraction this project avoids building ahead of need.

`NativeRunner` execs the game directly, no build, no provisioning.
`ProtonRunner` and `WineRunner` both provision a prefix and both had a real,
non-obvious bug caught by actually running them rather than trusting the
docs: **neither's exit code is a reliable success signal** — `umu-run`
returns 1 even after successfully initialising a prefix (it's reporting
"no game to launch", which is expected since provisioning deliberately
passes none), and `wine wineboot -u` returns 0 even when it did nothing
(e.g. the prefix directory didn't exist yet). Both runners instead check
whether `drive_c` actually appeared on disk — the only signal that means
what it says. `ProtonRunner::Discover` also checks `umu-run` itself is on
`PATH` before reporting any Proton build as usable, not just that the
build directory exists.

`RunnerRegistry` owns one instance of each runner and turns a `"kind:name"`
reference into a concrete runner + build — `"latest"`/`"auto"` resolve by
comparing each build's `version` (the Proton/Wine build's own version
string; a plain integer timestamp for Proton builds, sorted numerically).
`"auto"` specifically (`default_runner.windows`'s default) means "umu if a
Proton build is installed, else Wine, else broken with a clear reason" —
implemented in `RunnerRegistry::ProvisionGame`, the one place resolution
actually happens. Provisioning always pins the concrete resolved build onto
`runner_ref` (never `"latest"`) so a Proton/Wine update afterwards can't
silently change a working game's runtime. `library::Scanner` calls it
synchronously right after `AutoSetup` stores a new Windows game — like
scanning itself, this blocks the calling thread for real wall-clock time
(several seconds; it's genuinely initialising Proton/Wine) because there is
no job queue yet to move it off-thread. It's also called on demand from
`POST /v1/games/{id}/run` for a `needs_install`/`setting_up` game that has
no usable prefix yet, and retried by `Scanner` for any already-known game
still stuck at `setting_up` on a later scan — neither a Windows game auto-
provisioned at detection time nor "provisioned exactly once" holds
universally any more (see `docs/api.md` and the `TryProvision` comment in
`Scanner.cpp` for why: auto_setup can be off then turned on, a first
attempt can fail transiently, or the daemon can restart mid-provision).

A fourth runner, `SteamRunner` (`src/runner/SteamRunner.cpp`), doesn't fit
the "installed build you choose" model the other three share at all:
`UsesBuilds()` is `false`, and which Proton build to use isn't a choice —
it's resolved fresh from `compat_data_dir/config_info` at `BuildCommand`
time, exactly matching whatever Steam itself already set that prefix up
with. See `docs/api.md`'s Steam section for the full picture, including
why the *default* way to launch a Steam game (`steam.launch_mode:
"steam"`) never calls `BuildCommand` at all.

## Built: the frontend

`frontend/MiradClient.{h,cpp}` is the only thing in `mira-gui` that speaks to
`mirad` — plain httplib over the Unix socket plus nlohmann::json, mirroring
`mira`'s own client (`src/cli/main.cpp`), never `mira_core`. Every call is a
static method that spawns a worker thread, blocks there, then delivers the
result back on the caller's thread via `QMetaObject::invokeMethod(context,
..., Qt::QueuedConnection)` — safe even if `context` (the widget that asked)
is destroyed before the request finishes, since Qt just drops a queued call
whose context object is gone rather than invoking it. `EventStream` is the
one stateful exception: a long-lived connection to `GET /v1/events`,
reconnecting on any drop with a fixed backoff and replaying via
`Last-Event-ID` so a reconnect doesn't miss anything still in `mirad`'s
500-event buffer.

There are two library views, and both are kept on purpose. `LibraryWindow`
is the one that opens by default: a cover-art grid modelled on Playnite's
shelf, with a filter sidebar, a search box and a details panel. `MainWindow`
is the table — every field of every game visible at once — reachable as
`mira-gui --classic` or from the grid's View menu, which opens it as a second
top-level window rather than swapping the grid out. The grid is the better
browser and the table is the better audit tool for a library that was just
scanned, so neither replaces the other. Both are thin clients over the same
`MiradClient` calls and each holds its own `EventStream`, so two open windows
can't disagree about what the daemon said.

`LibraryWindow` fetches the whole library once (no `?status=` filter) and
filters it client-side in `ApplyFilter`. That's what lets the search box feel
instant, and it's the only way "Playing now" and "Never played" can be
filters at all — neither is a server-side query. One click selects a tile and
fills the details panel; a second (double) click launches; right-click opens
the per-game menu (play/stop, details & settings, open install folder,
remove). Launching on the *first* click would turn a misclick into a started
game, so the details panel is always what a single click produces. The tile
itself is painted by `GameTileDelegate` straight from the item's own roles,
so a repaint never reaches back into the window's state. `CoverArt.h`
generates the artwork: the backend has none yet, and a grid of identical grey
rectangles is unusable, so each game gets a hue derived from its *id* — stable
across restarts, renames and machines, so a user can learn a tile by sight.
When real artwork lands it becomes the fallback for games that have none.

`MainWindow` lists the library (`games_table_`) and rescans
(`POST /v1/library/scan` then `GET /v1/games`) once on launch. It never
re-fetches the whole list in response to a live event: `HandleGameEvent`
patches the specific row directly from each event's own payload —
`game.added`/`game.updated` carry the full record (`Server.cpp`,
`AutoSetup.cpp`), so `UpsertRow` needs nothing else; `game.removed` carries
only an id, so `RemoveRow` just deletes that row. Deliberately no idle-cost
fallback (no periodic reconciliation poll) — an occasional missed event was
judged cheaper than a timer that wakes up forever on the off chance one
was dropped (see "Idle cost" above for why that trade-off matters here).
Status and confidence are rendered through `GameColors.h`'s shared
`StatusColor`/`ConfidenceColor` helpers (one color per `GameStatus`,
red-to-green confidence gradient) so a game reads the same way here and in
the detail dialog.

`GameDetailDialog` (opened by double-clicking a row) loads the full record
via `GET /v1/games/{id}` and saves corrections via `PATCH /v1/games/{id}`.
The executable field is an editable combo box listing every detected
candidate (`GameDetail::Candidate`, labeled with its kind and score,
flagged when `is_installer` is set) alongside a "Browse…" button that opens
the desktop's native file picker (`QFileDialog`) and stores the result
relative to the game's `install_path` when possible. The runner field is
the same pattern sourced from `GET /v1/runners` — a reference like
`"proton_umu:GE-Proton11-7"` reads as a real name instead of raw text, while
staying editable for a runner `GET /v1/runners` won't list (`native`, which
has no concept of installed "builds").

`SettingsDialog` is generated entirely from `GET /v1/config/schema` — almost
no setting name is hardcoded anywhere in the frontend. One row per schema
entry (a checkbox for `Type::Bool`, a line edit otherwise), grouped by
`tier`: `basic` shown by default, `advanced`/`expert` behind a single
disclosure toggle, matching `api.md`'s guidance to never omit them outright.
Saving diffs each field's current text against the value it loaded with and
sends only the changed keys in one `PATCH /v1/config` — a dialog left open
and then saved unedited is a no-op, not a resend of every setting. A
checkbox has no reset button (its default is always one click away); every
other row does (`POST /v1/config/reset?key=...`). The one deliberate
exception to "no hardcoded setting names": `default_runner.windows`/
`default_runner.native` get the same runner-picker combo as
`GameDetailDialog`'s runner field instead of a plain line edit — the schema
has no way to mark a string as "this one's a runner reference," so
`SettingsDialog.cpp`'s `kRunnerKeys` names those two keys directly rather
than leaving them as free text a user has to already know the right syntax
for.

`MiradClient::ResolveSocketPath()` checks `$MIRA_SOCKET` before falling back
to the default `$XDG_RUNTIME_DIR/mira/mirad.sock` — the same override
`mirad --socket` takes on the daemon side. Without it, a `mirad` launched
without the dev sandbox's `XDG_CONFIG_HOME` (e.g. typed as a bare `mirad` in
a stray terminal) binds to that same default path and silently takes over
the socket a sandboxed dev daemon was already serving on, since
`XDG_CONFIG_HOME` only separates *config*, not the socket. Running the dev
daemon with its own `--socket` (e.g. `~/.mira-dev/mirad.sock`) and the
frontend with the matching `$MIRA_SOCKET` makes that collision structurally
impossible instead of just unlikely.

## Planned, not yet built

Recorded here so intent isn't lost between sessions:

- **Winetricks integration** — a `winetricks_verbs` list in a game's
  `runner_config` (already a free-form JSON blob in the schema) run against
  a fresh prefix before it's marked `ready`, with a `winetricks_defaults`
  setting seeding sane baseline verbs (corefonts, vcrun, DXVK) for every new
  prefix. `prefix/IPrefixProvider` (template-clone vs. plain init) is the
  other still-open piece of provisioning.
- **Exe-location enrichment for a Steam title's real launch command.**
  Steam Integration (`src/steam/`) reads `appmanifest_<id>.acf` and
  `compatdata/<id>/config_info` directly — enough to detect the app, find
  its install dir, and resolve exactly which Proton build/prefix Steam set
  up. What it deliberately does *not* do is read Steam's own cached
  `appinfo.vdf` for the official per-OS launch executable, so a detected
  Steam game's `exe_path` stays empty until set manually (see
  `SteamRunner::BuildCommand`'s error message, and `docs/api.md`'s
  `POST /v1/steam/scan`) — `steam.launch_mode: "steam"` (the default) never
  needs it at all, only `"direct"` does. Parsing `appinfo.vdf` to fill this
  in automatically is still open. For non-Steam titles, PCGamingWiki's
  public query API is a candidate second source; strictly best-effort on
  top of the offline heuristic detector either way, never a requirement —
  the zero-config test must keep passing with the network disabled.
- **`DELETE /v1/runners/{reference}`**, to remove an installed build.
  Downloading and listing (`GET /v1/runners/catalog`,
  `POST /v1/runners/download` — see `docs/api.md`) are built; nothing
  removes one yet, short of deleting its directory by hand.
- **`mirad --scan-once`** for the run-once-and-never-again persona described
  above.
- **`DaemonSupervisor` in `mira-gui`** for path 2 above.
- **Play-time, launch history, and (Steam-only) achievements.** `model::Game`
  already has an aggregate `play_seconds` counter (a fast-path total the API
  can return without reading a log), but the detailed history does not exist
  yet. Plan: a separate `stats.toml` alongside `settings.toml`/`games.toml`,
  holding one `[[session]]` entry per launch (`game_id`, `started_at`,
  `ended_at`, `duration_seconds`), written by `ProcessSupervisor` when a
  launched game exits (see `proc/ProcessSupervisor`, not yet built). Kept
  deliberately separate from `games.toml`: that file is meant to stay a
  clean, hand-editable *configuration* document, and an ever-growing
  session log doesn't belong mixed into it — but it still lives in the same
  `~/.config/mira` directory, so "one folder to back up" still holds.
  Achievements are Steam-specific and off the critical path entirely: Steam's
  official `ISteamUserStats/GetPlayerAchievements` Web API can return them,
  but only given a user-supplied Steam Web API key and SteamID64 (both
  optional settings, unset by default) and only for a game with a known
  Steam AppID and public stats. With no key configured, achievements are
  simply absent from the API response — never an error, never a blocker for
  everything else. Non-Steam games have no equivalent source and won't show
  achievement data.
