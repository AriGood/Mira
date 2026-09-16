# `mira` — command-line client

`mira` is a **pure REST client over the same Unix socket the frontend
uses** — identical endpoints, identical JSON, no direct access to
`settings.toml`/`games.toml`, no shortcut `mirad` doesn't also expose. See
`docs/architecture.md` for why that's a deliberate constraint rather than
an oversight: this CLI is the first consumer of the frontend's API, so a
missing field or an awkward endpoint shows up here first, while it's still
cheap to fix.

Every command below talks to `mirad` over `$XDG_RUNTIME_DIR/mira/mirad.sock`
(or whatever `socket_path` is set to); nothing works without a running
daemon except `mira daemon` itself, which starts one.

```sh
mirad --foreground &
mira status
```

---

## `mira status`
Checks whether `mirad` is reachable — `GET /v1/health`. Prints "mirad is
running" or an error naming the socket path it tried and suggesting how to
start the daemon (`systemctl --user status mirad` or `mirad --foreground`).

## `mira daemon [args...]`
Execs `mirad` (found next to this binary, or on `PATH`) with any given
arguments, replacing the `mira` process. Not an API call — nothing is
listening yet. Equivalent to just running `mirad` directly; exists mainly
so `mira daemon --foreground` is discoverable from the one tool a new user
already knows to reach for.

## `mira scan`
Triggers `POST /v1/library/scan` — walks every enabled library root right
now rather than waiting for `mirad`'s own inotify watcher to notice.
Prints the summary: `added: N  missing: N  restored: N`. Useful right
after changing `library_roots` (the watcher needs a restart to pick up new
roots; this doesn't).

## `mira list [--status S]`
`GET /v1/games`, optionally filtered to one status
(`setting_up | ready | broken | missing`). One line per game:
```
celeste                  ready      [unreviewed] Celeste
hollow-knight             setting_up             Hollow Knight  (windows)
```
`[unreviewed]` marks a game nobody has corrected since auto-setup — see
`docs/api.md` on `confidence`/`reviewed`.

## `mira show <id> [--effective]`
Without `--effective`: the full stored record for one game
(`GET /v1/games/{id}`) — status, exe path, candidates the detector
considered, everything. With `--effective`: `GET /v1/games/{id}/config`,
every global setting as it resolves *for this specific game* (its own
overrides, then `settings.toml`, then the built-in default), each tagged
with which layer supplied it. Both print formatted JSON.

## `mira set <id> [flags...]`
Corrects a game's configuration. Two independent kinds of flag, sent as two
separate requests (only the ones you use fire):

| Flag | Goes to |
|---|---|
| `--name`, `--exe`, `--args`, `--runner kind:name`, `--data-dir`, `--env KEY=VALUE` (repeatable) | `PATCH /v1/games/{id}` — the game's own fields |
| `--override dotted.key=value` (repeatable), `--unset dotted.key` (repeatable) | `PATCH /v1/games/{id}/config` — this game's overrides of a global setting |

```sh
mira set celeste --exe Celeste.exe --runner wine:system
mira set celeste --override scan.max_depth=8
mira set celeste --unset scan.max_depth        # back to inherited
```
`--override`'s value is parsed as JSON first (so `8`, `true`, `"a string"`
all work as typed), falling back to a bare string if it doesn't parse —
`--override detect.name_match_bonus=4.5` and `--override runner_ref=foo`
both do the right thing without quoting. Setting any game-field flag marks
the game reviewed; overrides don't (they're a separate concern — see
`docs/api.md`'s note on why the two endpoints are split).

## `mira config get|set|list|reset`
- `get <key>` — one value from `GET /v1/config` (dotted key, e.g.
  `scan.debounce_ms`).
- `set <key> <value>` — `PATCH /v1/config` with that one key. Value is
  parsed as JSON first, same fallback-to-string rule as `--override` above.
- `list` — every setting from `GET /v1/config/schema`: key, type, tier,
  one-line doc. This is the whole settings reference; there's no need to
  cross-reference `docs/api.md`'s schema section by hand.
- `reset [key]` — `POST /v1/config/reset`, one key or everything.

## `mira watch`
Tails `GET /v1/events` (SSE) forever, printing each event as it arrives.
Its entire implementation is a streaming `Get` that splits the response on
blank lines — read `CmdWatch` in `src/cli/main.cpp` before implementing the
frontend's equivalent; it's the whole pattern in about fifteen lines,
including surviving an indefinitely long gap between events (the client's
read timeout is set to a year, since `mirad` sends nothing at all while
idle — see `docs/architecture.md` on why that's deliberate, not an
oversight).

---

## Not yet implemented

`mira` has no `launch`/`stop`/`resetup`/`runners` commands yet because the
API endpoints they'd call don't exist yet either (`docs/api.md` marks them
**planned**) — the runner layer they depend on isn't built. They'll land
together.
