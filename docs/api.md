# API reference

REST over HTTP/1.1, served on a Unix domain socket — never TCP, never a
network-reachable port. Default socket: `$XDG_RUNTIME_DIR/mira/mirad.sock`
(overridable via the `socket_path` setting or `mirad --socket <path>`).

This document mirrors `Server::RegisterRoutes()` in `src/api/Server.cpp` — if
the two disagree, the code is correct and this file is stale. See
`docs/architecture.md` for *why* this API is the only contract between the
backend and every client, the frontend included.

All request/response bodies are JSON. All errors use one envelope:

```json
{ "error": { "code": "invalid_setting", "message": "scan.debounce_ms: must be between 0 and 600000" } }
```

`code` is stable and machine-readable; `message` is what a UI should show a
human. Endpoints marked **(planned)** don't exist yet and return 404 — see
`docs/architecture.md`'s "Planned, not yet built".

```sh
mirad &
mira status                 # or: curl --unix-socket "$XDG_RUNTIME_DIR/mira/mirad.sock" http://localhost/v1/health
```

---

## Health

### `GET /v1/health` — implemented
Liveness check; returns `{"status": "ok"}` if the daemon can respond at all.

---

## Settings

Backed by `settings.toml`. Every key is declared once in
`src/config/Schema.cpp`; `/v1/config/schema` reflects that registry directly,
which is what lets a frontend build a complete settings screen with zero
hardcoded knowledge of what settings exist.

### `GET /v1/config` — implemented
Every backend setting at its current value, plus the opaque `frontend` table
the backend stores but never interprets (see `docs/architecture.md`).

### `GET /v1/config/schema` — implemented
Every setting's type, default, tier (`basic | advanced | expert` — a
frontend should show `basic` and fold the rest behind a disclosure, never
omit them), and one-line doc string:
```json
[{ "key": "scan.debounce_ms", "type": "an integer", "default": 3000,
   "tier": "advanced", "doc": "How long a new folder must stop changing before it is scanned." }]
```

### `PATCH /v1/config` — implemented
Sets any subset of settings (nested, matching `GET /v1/config`'s shape) plus
optionally a `frontend` key. Validated against the schema before anything is
written — a bad value anywhere in the patch means nothing in it is applied.

### `POST /v1/config/reset[?key=<dotted.key>]` — implemented
Resets one key to its schema default, or everything if `key` is omitted.

---

## Games

Backed by `games.toml`. A game's `id` is a human-readable slug
(`"celeste"`, `"celeste-2"` on collision), not an opaque integer.

### `GET /v1/games[?status=<status>][?tag=<tag>]` — implemented
Lists games, optionally filtered to one `status`
(`setting_up | ready | broken | missing | needs_install`) and/or one tag
(a game must have that exact tag in its `tags` array). `"hidden"` is the
one tag this endpoint treats specially, and the only "hidden games"
concept there is — no separate field: a game tagged `hidden` is left out
of the list whenever `?tag=` isn't given, so `?tag=hidden` is how you list
exactly the hidden ones (and `?tag=hidden&status=ready` etc. compose
normally). `scan.tag_by_root` (default on) auto-tags every newly-detected
game with the name of the library root folder it was found in, so
multiple `library_roots` stay filterable without tagging anything by
hand — see `PATCH` below for setting tags by hand too.

### `GET /v1/games/{id}` — implemented
The full stored record for one game:
```json
{
  "id": "celeste", "install_path": "/home/x/Games/Celeste", "name": "Celeste",
  "status": "ready", "confidence": 0.9, "reviewed": false, "platform": "native",
  "exe_path": "Celeste", "args": "", "working_dir": "", "runner_ref": "",
  "data_dir": "", "runner_config": {}, "overrides": {}, "last_error": "",
  "created_at": 0, "updated_at": 0, "last_played_at": null, "play_seconds": 0,
  "env": {}, "candidates": [], "tags": []
}
```
`confidence`/`reviewed` are how auto-setup stays safe without blocking on a
human: the backend always commits to its best guess, and the frontend can
sort "nobody has double-checked this" to the top rather than gating on it.
`candidates` lists every executable the detector considered, so the frontend
can offer "use this one instead" without a re-scan. `runner_config` is
opaque here — owned by whichever runner `runner_ref` names, never by core.

### `PATCH /v1/games/{id}` — implemented
Corrects the game's own fields: any of `name`, `exe_path`, `args`,
`working_dir`, `runner_ref`, `data_dir` (where its prefix/data lives — see
`docs/architecture.md` on why it isn't called `prefix_path`),
`runner_config` (merged, not replaced), `env` (merged), `tags` (replaced
wholesale — a plain list has no natural per-entry merge the way `env`'s
null-removes-a-key convention does, so send the full set you want; `[]`
clears it). Setting any of these marks the game `reviewed: true` — a
correction *is* the review. Never touches `overrides` (see `.../config`
below — a game's own fields and its overrides of unrelated global settings
are different concerns and don't share a request body). Publishes
`game.updated`. 404 if the id is unknown.

### `DELETE /v1/games/{id}[?delete_files=true][?delete_prefix=true]` — implemented
Forgets the game. By default never touches disk. `delete_files=true` also
removes `install_path` (the game's own folder); `delete_prefix=true` also
removes `data_dir` (its Wine/Proton prefix, if any). Both are restricted to
paths that actually resolve inside a configured `library_roots`/
`prefix_root` — never wherever a hand-edited `games.toml` happens to say.
Publishes `game.removed`.

### `GET /v1/games/{id}/config` — implemented
Every schema key resolved through `default -> settings.toml -> this game's
overrides`, tagged with which layer supplied it — mirrors `GET /v1/config`
but scoped to one game:
```json
{ "scan.max_depth": { "value": 8, "layer": "game", "overridable": true },
  "library_roots": { "value": ["~/Games"], "layer": "default", "overridable": false } }
```
This is what lets a frontend show "Runner: GE-Proton11-7 *(default)*" next
to a reset button, without separately tracking where each value came from.

### `PATCH /v1/games/{id}/config` — implemented
Sets or removes this game's overrides of global settings: a flat
`{"dotted.key": value}` body, where a `null` value removes that override.
Rejects (with nothing applied) if a key isn't overridable — some settings,
like `library_roots`, describe the daemon rather than a game, see
`config::Resolver::IsOverridable` — or if a value fails schema validation.

### `POST /v1/games/{id}/launch` — implemented
Resolves `runner_ref` (defaulting to `native:native`) and execs the game,
wrapped by `command_wrappers` in order (first entry outermost), tracked by
`proc::ProcessSupervisor` for crash detection and playtime. 404 if unknown,
409 if `needs_install` or not `ready`.

`launch.pre_script` (global default, overridable per game via `.../config`)
runs first, via `sh -c`, and blocks the request — a non-zero exit aborts
the launch entirely with `409 pre_launch_failed` and the script's own
output as the error, so a script that's supposed to prepare something the
game needs (mount a drive, set a CPU governor) actually gets to finish
before the game starts. `launch.post_script` runs once the game process
exits (clean, crashed, or stopped, always) — in the background, so it
never blocks anything, and its own exit code is only logged, never
reflected in the recorded playtime/crash state.

A Steam-sourced game (`runner_ref` starting `steam:`) is a special case:
if the effective `steam.launch_mode` is `"steam"` — the default — this
instead fires `steam steam://rungameid/<appid>` and returns immediately.
Mira didn't spawn that process, so `proc::ProcessSupervisor::Launch`'s
normal `waitpid()`-based tracking can't apply to it; instead, if
`steam.track_process` is on (the default), a background watcher polls
`/proc` for a process carrying `SteamAppId`/`SteamGameId=<appid>` in its
environment — the same variable the Steamworks API itself reads — so
Mira still shows the game `running` and records playtime, just without a
real exit code/signal (not obtainable for a process Mira didn't spawn;
Steam's own client already has that). `launch.post_script` still runs
once it's gone. Set `steam.launch_mode` to `"direct"` (globally or per
game) to have Mira exec it itself instead, through the same Proton build
and prefix Steam already set up — normal tracking applies, but `exe_path`
has to be set manually first (see `POST /v1/steam/scan` below for why
Mira can't determine it on its own).

### `POST /v1/games/{id}/stop` — implemented
Sends SIGTERM to the whole process group, escalating to SIGKILL after
`launch.stop_timeout_s` if it's still running. 409 if not running.

### `POST /v1/games/{id}/run` — implemented
Body: `{"exe_path": "...", "args": "..."}`. Runs that exe inside this
game's own prefix — normal ProcessSupervisor tracking, same as `/launch`,
but the exe/args come from the request instead of the stored game.
Provisions a prefix on demand if there isn't a usable one yet (a
`needs_install` game never gets one from Scanner, since it never
auto-provisions an installer). This is how a `needs_install` game's
installer actually gets run, and doubles as the general "run something in
this prefix" escape hatch — for a known package/DLL fix rather than an
arbitrary exe, `/tricks` below is the better fit.

### `POST /v1/games/{id}/finish-install` — implemented
The other half of the `needs_install` escape hatch: after running the
installer via `/run` and `PATCH`ing `exe_path` to whatever it actually
produced, this flips status to `ready`. 409 if `exe_path` is still empty.

### `POST /v1/games/{id}/tricks` — implemented
Body: `{"verb": "corefonts"}`. Runs `winetricks --unattended <verb>`
against this game's own Wine/Proton prefix — the real `winetricks` tool
shelled out to, not reimplemented (its value is hundreds of crowdsourced,
constantly-updated verb definitions, not the script itself; see
`docs/architecture.md`, Replaceability). 404s with a specific error if the
game has no prefix yet, the prefix was never provisioned, the runner isn't
Wine/Proton-based (native games, or a Steam game that turned out to be
native), or `winetricks` itself isn't on `PATH`. Runs in the background
(a verb can mean downloading and installing a real redistributable —
minutes, not request-scale) and returns `202` immediately;
`tricks.started` / `.finished` / `.failed` on the event stream track it.

---

## Library

`library_roots` (the folders being watched) is a plain setting, managed
through `GET`/`PATCH /v1/config` like any other — there is no separate
roots resource, because a plain string array is all the current design
needs; see `docs/architecture.md` if that stops being true.

### `POST /v1/library/scan` — implemented
Walks every enabled library root immediately: detects new game folders,
auto-configures and stores them (publishing `game.added` for each), and
marks previously-known games whose folder disappeared as `missing`. Runs
synchronously and returns a summary rather than a job id — there is no
worker/job queue yet, and a scan of a normal-sized library finishes well
within one HTTP request:
```json
{ "added": 1, "missing": 0, "restored": 0 }
```
This is the on-demand counterpart to automatic detection — `mirad` also
watches every root continuously via inotify (see
`library::Watcher`/`docs/architecture.md`) and calls this same scan logic
itself once a new folder's contents stop changing, with no request needed.
A change to `library_roots` needs a daemon restart to be picked up by the
watcher; this endpoint works immediately either way.

---

## Runners

### `GET /v1/runners` — implemented
Every installed build of every runner kind, freshly discovered on each
call (no caching, no refresh endpoint needed as a result):
```json
[{ "kind": "proton", "name": "GE-Proton11-7", "path": "/home/x/.steam/steam/compatibilitytools.d/GE-Proton11-7-x86_64",
   "version": "1789520217", "reference": "proton:GE-Proton11-7" }]
```
`reference` is what a game's `runner_ref` field and `default_runner.*`
settings use. `native` never appears here — it has no concept of "builds".
`steam` (see below) never appears here either — its "build" is whatever
Steam itself set a given prefix up with, resolved per-game, not a
general-purpose installed build the registry tracks.

### `GET /v1/runners/catalog?kind=proton|wine` — implemented
What's *available to install*, not what's installed (that's `/v1/runners`
above) — lists releases from the source configured in
`runner_sources.proton_ge.*`/`runner_sources.wine_ge.*`, newest first, live
against the GitHub API (so this one has real network latency, unlike
everything else in this file):
```json
[{ "tag": "GE-Proton11-7", "asset_name": "GE-Proton11-7-x86_64.tar.gz",
   "size_bytes": 563784602, "published_at": "2026-09-16T02:28:16Z", "has_checksum": true }]
```

### `POST /v1/runners/download` — implemented
Body: `{"kind": "proton"|"wine", "tag": "..."}` (a tag from the catalog
above). Downloads and installs it into `runner_search_paths[0]` /
`wine_search_paths[0]`, verifying its checksum first if the release
shipped one — a mismatch discards the download rather than installing it.
Runs detached (a build can be 500+ MB; no job queue yet, see
`docs/architecture.md`) and returns `202` immediately. Progress is on the
event stream: `runners.download.started` / `.finished` / `.failed`.

### `DELETE /v1/runners/{kind}:{name}` — implemented
Uninstalls a build fetched via `/v1/runners/download` — the other half of
catalog/download; discovery (`GET /v1/runners`) picks the removal up on the
next call, nothing separate to update. Deletes only a path that both
resolves to this exact build and really sits inside a configured
`runner_search_paths`/`wine_search_paths` entry — same containment check
`DELETE /v1/games/{id}` uses, so `wine:system` (the real system binary,
found on `PATH`) 400s rather than being deleted. `400` for `auto`/`latest`
(name a concrete build) or a kind with no separate builds at all (`native`,
`steam`); `404` if that build isn't actually installed.

### `GET /v1/runners/{kind}/schema` — implemented
What `game.runner_config` accepts for one kind, so a frontend can render it
generically instead of hardcoding per-runner knowledge (see
`docs/architecture.md`, Replaceability):
```json
[{ "key": "gameid", "type": "string", "doc": "Steam AppID umu should report..." }]
```
`[]` for a kind with no fields — every kind but `proton` today. `404` for
an unknown kind.

### `POST /v1/runners/refresh` — planned (not needed: `GET /v1/runners`
already discovers fresh on every call, with no caching to invalidate — see
above)

---

## Steam

Detected Steam games are ordinary entries in `games.toml`/`GET /v1/games`
(`runner_ref` starting `"steam:<appid>"`) — every other games endpoint
already works on one unmodified. This section only covers what's actually
Steam-specific.

### `POST /v1/steam/scan` — implemented
Detects installed Steam apps (reading Steam's own `libraryfolders.vdf`/
`appmanifest_*.acf`/`compatdata/<id>/config_info` files directly, not
asking a possibly-not-running Steam client) and upserts them:
```json
{ "added": 2, "updated": 0 }
```
Idempotent — rescanning updates Steam-owned fields (`name`, `install_path`,
`data_dir`) without touching anything the user configured (`exe_path`,
`args`, `env`, overrides). `exe_path` is never set by this scan: Steam
resolves the real launch command from its own `appinfo` cache, which isn't
readable from disk, so it's only known if set manually — relevant only to
`steam.launch_mode: "direct"` (see `/launch` above), since the default
`"steam"` mode doesn't need it at all.

---

## Metadata

Cover art and store info, fetched from public web APIs and cached on disk
next to `settings.toml` (`metadata/<id>.json`, `artwork/<id>/cover.*`) —
never written into `games.toml`, since none of it is user-editable state and
it can always be re-fetched. Two sources, picked by whether a game is
Steam-owned (`runner_ref` starting `"steam:"`):

- **Steam-owned**: Steam's own public store API (`store.steampowered.com`)
  for description, genres, categories, release date, developers/publishers,
  price, metacritic score, website; Steam's public review-summary endpoint
  for the aggregate score; [ProtonDB](https://www.protondb.com)'s
  compatibility tier; cover art from Steam's own CDN. None of these need a
  key.
- **Everything else**: [SteamGridDB](https://www.steamgriddb.com), matched
  by name search, for cover art only — there is no equivalent free metadata
  source for a non-Steam game. Needs `steamgriddb.api_key` set; silently
  skipped without one.

Fetched automatically the moment a game is first detected (`POST
/v1/library/scan`, the inotify watcher, and `POST /v1/steam/scan` all
trigger it for newly-added games only — never re-fetched on every rescan of
an already-known game) via a small in-process queue that bounds every
outstanding fetch to the daemon's own lifetime, so a slow or unreachable
source never blocks a scan. Controlled by `metadata.enabled` (default on).

### `GET /v1/games/{id}/metadata` — implemented
The cached JSON verbatim, `{"source": "steam"|"steamgriddb", "fetched_at":
..., "steam": {...}, "steam_reviews": {...}, "protondb": {...}, "artwork":
{...}}` — every top-level key besides `source`/`fetched_at`/`artwork` is
present only if that source actually returned something. `404` means either
"never fetched" or "fetched, found nothing" — `POST .../metadata/refresh`
below disambiguates by trying again.

### `GET /v1/games/{id}/artwork` — implemented
The cached cover image itself (`image/jpeg` or `image/png`, whatever the
source sent), read straight off disk. `404` if nothing's cached yet.

### `POST /v1/games/{id}/metadata/refresh` — implemented
Re-runs the fetch for one game on demand — a `steamgriddb.api_key` was just
set, or the first automatic attempt failed transiently. Bypasses
`metadata.enabled` (an explicit request should work even with automatic
fetching off). Runs in the background the same way the automatic fetch
does; returns `202` immediately. `game.metadata_ready`/`.metadata_failed` on
the event stream say when it's done.

---

## Events

### `GET /v1/events` — implemented
Server-Sent Events. Each event:
```
id: 42
event: game.updated
data: {"id":"celeste","name":"Celeste (Steam)", ...}

```
Reconnect with a `Last-Event-ID` header to replay everything published
since that id — the buffer holds the last 500 events, enough to survive a
frontend restart, but **not** a daemon restart (events are in-memory only;
`docs/architecture.md` explains why that trade-off is deliberate).

Published today:
- `game.added` — fires the moment a new folder is auto-configured,
  carrying the full detected configuration plus `open_config: <bool>` from
  the `open_config_on_add` setting, so the frontend knows whether to raise
  its config menu immediately.
- `game.updated`, `game.removed`.
- `game.state` — `{"id": ..., "state": "running" | "exited" | "crashed"}`,
  from `proc::ProcessSupervisor` (a Steam game launched via
  `steam.launch_mode: "steam"` never emits this — Mira isn't tracking its
  process; see `/launch` above).
- `game.launched` — `{"id": ..., "via": "steam"}`, the untracked
  counterpart to `game.state` for that same case.
- `runners.download.started` / `.finished` / `.failed` — see
  `POST /v1/runners/download` above.

Planned as the rest of the backend lands: `scan.started`, `scan.finished`,
`setup.progress`, `setup.finished`, `setup.failed`, `config.changed`.

`mira watch` (`src/cli/main.cpp`) is the reference client — its whole
implementation is a streaming `Get` split on blank lines, worth reading
before building the frontend's equivalent.

---

## Jobs

For work long enough that it shouldn't block a request — not scanning
(synchronous today, see above), but provisioning a prefix once the runner
layer exists. `GET /v1/jobs/{id}` — **planned**, alongside that work.
