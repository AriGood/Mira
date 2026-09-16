# API reference

REST over HTTP/1.1, served on a Unix domain socket — never TCP, never a
network-reachable port. Default socket: `$XDG_RUNTIME_DIR/mira/mirad.sock`
(overridable via the `socket_path` setting or `mirad --socket <path>`).

This document describes the surface `mirad` actually serves. It is a mirror
of `Server::RegisterRoutes()` in `src/api/Server.cpp` — if the two disagree,
the code is correct and this file is stale. See `docs/architecture.md` for
*why* this API is the only contract between the backend and every client
(the frontend included).

All request/response bodies are JSON. All errors use one envelope:

```json
{ "error": { "code": "invalid_setting", "message": "scan.debounce_ms: must be between 0 and 600000" } }
```

`code` is stable and machine-readable; `message` is what a UI should show a
human.

Every endpoint below marked **(implemented)** exists today and is exercised
by an end-to-end test. Endpoints marked **(planned)** are part of the
design (see `docs/architecture.md`'s "Planned, not yet built") but return
404 today — they are listed here so the shape is settled before the
frontend is built against it.

## Trying it from a shell

```sh
mirad --foreground &
curl --unix-socket "$XDG_RUNTIME_DIR/mira/mirad.sock" http://localhost/v1/health
```

Or just use the CLI, which is a client for exactly this API:

```sh
mira status
mira config list
```

---

## Health

### `GET /v1/health` — implemented

```json
{ "status": "ok" }
```

---

## Settings

Backed by `settings.toml`; see `docs/architecture.md` for the on-disk format
and the `[frontend]` passthrough table. Every key is declared once in
`src/config/Schema.cpp`, which is what `/v1/config/schema` reflects.

### `GET /v1/config` — implemented

Returns the full settings document (every backend key, at whatever layer —
default or file — it currently resolves to) plus the opaque `frontend` table.

### `GET /v1/config/schema` — implemented

```json
[
  {
    "key": "scan.debounce_ms",
    "type": "an integer",
    "default": 3000,
    "tier": "advanced",
    "doc": "How long a new folder must stop changing before it is scanned. Raise it if games arrive over a slow network share."
  }
]
```

`tier` is `basic | advanced | expert` — a frontend generating a settings UI
from this should show `basic` by default and fold the rest behind a
disclosure, never omit them. This is the endpoint that lets the GUI have a
complete settings screen with zero hardcoded knowledge of what settings
exist.

### `PATCH /v1/config` — implemented

Body: any subset of settings, nested to match `GET /v1/config`'s shape
(e.g. `{"scan": {"debounce_ms": 5000}}`), plus optionally a `frontend` key
with whatever the frontend wants stored. Validated against the schema
before anything is written — a bad value in the patch means *nothing* in
the patch is applied, and the response is a 400 with the error envelope
above.

### `POST /v1/config/reset[?key=<dotted.key>]` — implemented

Resets one key to its schema default, or (with no `key`) resets everything.

---

## Games

Backed by `games.toml`; one entry per game, id is a human-readable slug
(`"celeste"`, `"celeste-2"` on collision), not an opaque integer.

### `GET /v1/games[?status=<status>]` — implemented

Array of games. `status` filters to one of `setting_up | ready | broken |
missing`.

### `GET /v1/games/{id}` — implemented

```json
{
  "id": "celeste",
  "library_root": "",
  "install_path": "/home/x/Games/Celeste",
  "name": "Celeste",
  "status": "ready",
  "confidence": 0.9,
  "reviewed": false,
  "platform": "native",
  "exe_path": "Celeste",
  "args": "",
  "working_dir": "",
  "runner_ref": "",
  "data_dir": "",
  "runner_config": {},
  "overrides": {},
  "last_error": "",
  "created_at": 0,
  "updated_at": 0,
  "last_played_at": null,
  "play_seconds": 0,
  "env": {},
  "candidates": []
}
```

`confidence` and `reviewed` are how auto-setup stays safe without blocking
on a human: the backend always commits to its best guess (once auto-setup
is built — see below), and the frontend can sort "things nobody has
double-checked" to the top rather than gating on them. `candidates` lists
every executable the detector considered, so the frontend can offer "no,
use this one instead" without a re-scan.

`runner_config` is intentionally opaque here: it is owned entirely by
whichever runner `runner_ref` names, validated against that runner's own
`GET /v1/runners/{kind}/schema` (planned), never by core.

### `PATCH /v1/games/{id}` — implemented

Body: any of `name`, `exe_path`, `args`, `working_dir`, `runner_ref`,
`runner_config` (merged, not replaced), `env` (merged). Setting any of
these marks the game `reviewed: true` — a correction *is* the review.
Publishes a `game.updated` event. 404 if the id doesn't exist.

### `DELETE /v1/games/{id}[?delete_data=true]` — implemented

Removes the game from `games.toml`. Never touches the game's actual files
on disk. Publishes `game.removed`.

`delete_data` is accepted but not yet wired to anything (deleting the
runner's data directory is a runner-specific operation that doesn't exist
yet — see Runners below); today the query parameter is ignored.

### `GET /v1/games/{id}/effective-config` — implemented

Every schema key resolved through `default -> settings.toml -> this game's
overrides`, each tagged with which layer supplied it:

```json
{
  "scan.max_depth": { "value": 8, "layer": "game", "overridable": true },
  "scan.debounce_ms": { "value": 3000, "layer": "default", "overridable": true },
  "library_roots": { "value": ["~/Games"], "layer": "default", "overridable": false }
}
```

This is what lets a frontend show "Runner: GE-Proton11-7 *(default)*" next
to a reset button, without separately tracking where each value came from.

### `PUT /v1/games/{id}/overrides/{key}` — implemented

Body: `{"value": <anything matching the key's schema type>}`. Sets a
per-game override; 400 if `key` isn't overridable (some settings, like
`library_roots`, describe the daemon rather than a game — see
`config::Resolver::IsOverridable`) or the value fails schema validation.

### `DELETE /v1/games/{id}/overrides/{key}` — implemented

Removes a per-game override, reverting to the inherited value.

### `POST /v1/games/{id}/launch` — planned
### `POST /v1/games/{id}/stop` — planned
### `POST /v1/games/{id}/resetup` — planned

Re-runs detection and provisioning from scratch — the escape hatch for when
auto-setup guessed badly wrong.

---

## Library

### `GET /v1/library/roots` — planned
### `POST /v1/library/roots` — planned
### `DELETE /v1/library/roots/{id}` — planned
### `POST /v1/library/scan` — planned

`library_roots` already exists as a setting (`GET/PATCH /v1/config`); these
dedicated endpoints are for the richer per-root state (enabled/disabled,
last-scanned) that doesn't fit a plain string array.

---

## Runners

### `GET /v1/runners` — planned
### `GET /v1/runners/{kind}/schema` — planned
### `POST /v1/runners/refresh` — planned

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
since that id — the buffer holds the last 500 events (`EventBus`'s default
capacity), enough to survive a frontend restart, but **not** a daemon
restart (events are in-memory only; see `docs/architecture.md` for why that
trade-off was made deliberately rather than persisting them).

Event types published today: `game.updated`, `game.removed`. Planned as the
rest of the backend lands: `scan.started`, `scan.finished`, `game.added`
(carrying the full auto-detected configuration plus `open_config: true`, so
the frontend knows to raise its config menu immediately), `setup.progress`,
`setup.finished`, `setup.failed`, `game.state` (`launching | running |
exited`), `runners.updated`, `config.changed`.

`mira watch` (in `src/cli/main.cpp`) is the reference client for this
endpoint — its entire implementation is a `Get` with a streaming content
receiver splitting on blank lines, worth reading before implementing the
frontend's equivalent.

---

## Jobs

Long-running work (a scan, provisioning a prefix) is meant to return `202`
with a job id rather than blocking the request. `GET /v1/jobs/{id}` —
**planned**, alongside the work it would track.
