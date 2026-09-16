# Architecture

## What this is

Mira is a Linux game launcher: native games plus Windows games via Wine/Proton.
It is three programs sharing one contract:

| Program    | What it is                                    | Role |
|------------|------------------------------------------------|------|
| `mirad`    | the backend daemon                             | owns all state, does all the work |
| `mira`     | a command-line client                          | a REST client over the same API the GUI uses |
| `mira-gui` | the Qt frontend (currently a blank skeleton)    | a REST client over the same API the CLI uses |

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
            settings.toml), Resolver (layered default -> file -> per-game
            lookup with provenance).
  model/    Game, RunnerBuild, Event, Candidate — plain structs plus
            ToJson/FromJson. No behaviour lives here.
  store/    GameStore — the games.toml-backed source of truth for the
            library.
  api/      EventBus (in-memory pub/sub) and Server (the REST routes) —
            see api.md for the surface this registers.
  cli/      main.cpp for `mira`.
  mirad_main.cpp   entry point for the daemon.

frontend/   the Qt skeleton (mira-gui). A separate CMake project scope;
            never add a target_link_libraries(mira-gui PRIVATE mira_core).

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

This is currently a documented design, not yet implemented — `mira-gui` is
still the blank skeleton described above. Recorded here because it is a
load-bearing decision for how the frontend is built, not an afterthought:
whoever implements this should add a small `DaemonSupervisor` in the
frontend that:

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
run it again: run `mirad --foreground` from a terminal, do the setup work
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
- Nothing is fetched, scanned, or recomputed on a timer. (The watcher/scan
  loop that will make this concrete is not built yet — see Open items.)

## Replaceability

Two seams are treated as real interfaces because they are known to need a
second implementation later; everything else stays concrete until it
doesn't:

- **Runners** (`IRunner`, not yet written) — `umu-run` is a means to a
  working Proton launch today, not a permanent dependency. The plan is to
  validate the interface the same way `NativeRunner` already proves the
  general shape: ship a second real implementation (`WineRunner`, plain
  system wine) before trusting the abstraction, and keep every umu-specific
  string inside one file.
- **Detection rules** — per-user heuristics that are certain to need
  retuning; `config/Schema.cpp`'s `detect.*` keys already externalize the
  weights this will use.

## Planned, not yet built

Recorded here so intent isn't lost between sessions:

- **`library/Detector.{h,cpp}`** — walks a game folder, scores candidate
  executables using the `detect.*` schema keys, assigns a confidence.
- **`library/Watcher.{h,cpp}`** — inotify-based, non-recursive per root,
  debounced via a timer so a folder mid-copy isn't scanned prematurely.
- **`library/AutoSetup.{h,cpp}`** — wires detection through to a provisioned
  game and fires `game.added` before provisioning finishes, so the frontend
  can open its config menu immediately.
- **`runner/{IRunner,NativeRunner,WineRunner,UmuRunner}`** and
  **`prefix/IPrefixProvider`** — provisioning, including winetricks
  integration: a `winetricks_verbs` list in a game's `runner_config` (already
  a free-form JSON blob in the schema) run against a fresh prefix before it's
  marked `ready`, with a `winetricks_defaults` setting seeding sane baseline
  verbs (corefonts, vcrun, DXVK) for every new prefix.
- **Exe-location enrichment** (optional, network-based, off the offline
  critical path): for a game that is a Steam title, read the local
  `appmanifest_<id>.acf` and Steam's own cached `appinfo.vdf` for the
  official per-OS launch executable — no network call, fully legitimate,
  high-value. For non-Steam titles, PCGamingWiki's public query API is a
  candidate second source. SteamDB is deliberately not a candidate source:
  it has no public API and explicitly discourages scraping. Either source is
  strictly best-effort enrichment on top of the offline heuristic detector,
  never a requirement — the zero-config test must keep passing with the
  network disabled.
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
