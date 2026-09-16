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
mirad --foreground &
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

### `GET /v1/games[?status=<status>]` — implemented
Lists games, optionally filtered to one `status`
(`setting_up | ready | broken | missing`).

### `GET /v1/games/{id}` — implemented
The full stored record for one game:
```json
{
  "id": "celeste", "install_path": "/home/x/Games/Celeste", "name": "Celeste",
  "status": "ready", "confidence": 0.9, "reviewed": false, "platform": "native",
  "exe_path": "Celeste", "args": "", "working_dir": "", "runner_ref": "",
  "data_dir": "", "runner_config": {}, "overrides": {}, "last_error": "",
  "created_at": 0, "updated_at": 0, "last_played_at": null, "play_seconds": 0,
  "env": {}, "candidates": []
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
`runner_config` (merged, not replaced), `env` (merged). Setting any of these
marks the game `reviewed: true` — a correction *is* the review. Never
touches `overrides` (see `.../config` below — a game's own fields and its
overrides of unrelated global settings are different concerns and don't
share a request body). Publishes `game.updated`. 404 if the id is unknown.

### `DELETE /v1/games/{id}[?delete_data=true]` — implemented
Forgets the game. Never touches its files on disk. Publishes `game.removed`.
`delete_data` is accepted but not yet wired to anything — deleting a
runner's data directory is runner-specific and the runner layer doesn't
exist yet, so today the parameter is a no-op.

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

### `POST /v1/games/{id}/launch` — planned
### `POST /v1/games/{id}/stop` — planned
### `POST /v1/games/{id}/resetup` — planned
Re-runs detection and provisioning from scratch — the escape hatch for when
auto-setup guessed badly wrong.

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
[{ "kind": "proton_umu", "name": "GE-Proton11-7", "path": "/home/x/.steam/steam/compatibilitytools.d/GE-Proton11-7-x86_64",
   "version": "1789520217", "reference": "proton_umu:GE-Proton11-7" }]
```
`reference` is what a game's `runner_ref` field and `default_runner.*`
settings use. `native` never appears here — it has no concept of "builds".

### `GET /v1/runners/{kind}/schema` — planned
### `POST /v1/runners/refresh` — planned (not needed today; see above)

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

Published today: `game.updated`, `game.removed`, `game.added` (fires the
moment a new folder is auto-configured, carrying the full detected
configuration plus `open_config: <bool>` from the `open_config_on_add`
setting, so the frontend knows whether to raise its config menu
immediately). Planned as the rest of the backend lands: `scan.started`,
`scan.finished`, `setup.progress`, `setup.finished`, `setup.failed`,
`game.state` (`launching | running | exited`), `runners.updated`,
`config.changed`.

`mira watch` (`src/cli/main.cpp`) is the reference client — its whole
implementation is a streaming `Get` split on blank lines, worth reading
before building the frontend's equivalent.

---

## Jobs

For work long enough that it shouldn't block a request — not scanning
(synchronous today, see above), but provisioning a prefix once the runner
layer exists. `GET /v1/jobs/{id}` — **planned**, alongside that work.
