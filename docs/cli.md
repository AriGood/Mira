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
mirad &
mira status
```

---

## `mira status`
Checks whether `mirad` is reachable — `GET /v1/health`. Prints "mirad is
running" or an error naming the socket path it tried and suggesting how to
start the daemon (`systemctl --user status mirad` or just running `mirad`).

## `mira daemon [args...]`
Execs `mirad` (found next to this binary, or on `PATH`) with any given
arguments, replacing the `mira` process. Not an API call — nothing is
listening yet. Equivalent to just running `mirad` directly; exists mainly
so `mira daemon` is discoverable from the one tool a new user
already knows to reach for.

## `mira setup [--enable-service] [--remove|--uninstall]`
Local, no API call. Run from the AppImage (`./Mira-x86_64.AppImage setup`).
Writes a `~/.local/bin/mira` wrapper (plus `mirad`/`mira-run` links) that
runs the binaries bundled in the AppImage, a desktop entry with an
"Uninstall Mira" action, the icon, and a systemd user unit.
`--enable-service` also enables the unit. `--remove` undoes it all;
`--uninstall` also asks for confirmation and deletes the AppImage. Games,
settings and prefixes are never touched.

## `mira scan`
Triggers `POST /v1/library/scan` — walks every enabled library root right
now rather than waiting for `mirad`'s own inotify watcher to notice.
Prints the summary: `added: N  missing: N  restored: N`. Useful right
after changing `library_roots` (the watcher needs a restart to pick up new
roots; this doesn't).

## `mira list [--status S] [--tag T]`
`GET /v1/games`, optionally filtered to one status
(`setting_up | ready | broken | missing | needs_install`) and/or one tag.
One line per game:
```
celeste                  ready      [unreviewed] Celeste
hollow-knight             setting_up             Hollow Knight  (windows)
```
`[unreviewed]` marks a game nobody has corrected since auto-setup — see
`docs/api.md` on `confidence`/`reviewed`. Games tagged `hidden` are left
out unless you pass `--tag hidden` explicitly — see `docs/api.md`'s Games
section for the whole tags/hidden story.

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
| `--name`, `--exe`, `--args`, `--runner kind:name`, `--data-dir`, `--env KEY=VALUE` (repeatable), `--tag NAME`/`--untag NAME` (repeatable) | `PATCH /v1/games/{id}` — the game's own fields |
| `--override dotted.key=value` (repeatable), `--unset dotted.key` (repeatable) | `PATCH /v1/games/{id}/config` — this game's overrides of a global setting |

```sh
mira set celeste --exe Celeste.exe --runner wine:system
mira set celeste --override scan.max_depth=8
mira set celeste --unset scan.max_depth        # back to inherited
mira set celeste --tag hidden                  # leaves it out of `mira list` by default
mira set celeste --untag hidden
```
`--tag`/`--untag` edit the *current* tag set (fetching it first, since the
API itself replaces the array wholesale — see `docs/api.md`) rather than
requiring you to retype every tag the game already has.
`--override`'s value is parsed as JSON first (so `8`, `true`, `"a string"`
all work as typed), falling back to a bare string if it doesn't parse —
`--override detect.name_match_bonus=4.5` and `--override runner_ref=foo`
both do the right thing without quoting. Setting any game-field flag marks
the game reviewed; overrides don't (they're a separate concern — see
`docs/api.md`'s note on why the two endpoints are split).

## `mira launch <id>` / `mira stop <id>`
`POST /v1/games/{id}/launch` / `/stop`. For most games this is a normal
tracked launch (crash detection, playtime). For a Steam-sourced game under
the default `steam.launch_mode`, `launch` instead fires
`steam://rungameid/<appid>` and returns immediately — see `docs/api.md`'s
`/launch` entry for the full explanation of why that one case isn't
tracked.

## `mira run <id> --exe PATH [--args ARGS]`
`POST /v1/games/{id}/run` — runs an arbitrary exe inside this game's own
prefix, tracked like a normal launch. Provisions a prefix first if it
doesn't have one yet. This is how a `needs_install` game's installer
actually gets run:
```sh
mira run my-game --exe UplayInstaller.exe   # run the installer
mira set my-game --exe MyGame.exe            # point at what it produced
mira finish-install my-game                  # mark it ready
```

## `mira finish-install <id>`
`POST /v1/games/{id}/finish-install` — the last step of the sequence
above: flips a `needs_install`/`broken` game to `ready` once `exe_path` has
been corrected. Fails if `exe_path` is empty, still the installer, or
doesn't exist.

## `mira install <id> [--interactive] [--installer PATH]`
`POST /v1/games/{id}/install` — runs a `needs_install` game's installer
and marks it ready once the game exe is found. Inno Setup/NSIS run
silently; anything else (or `--interactive`) opens so you can click
through it. `--installer` picks the installer by hand (also works for a
`broken` game).
- `mira install <id> --info [--installer PATH]` — `GET .../installer`:
  path, size, format, and the silent arguments.
- `mira install <id> --progress` — `GET .../install/progress`.

## `mira relocate <id>` / `mira library relocate`
`POST /v1/games/{id}/relocate` / `POST /v1/library/relocate` — move a
game's files and prefix into Mira's layout (named per `prefix_naming`).
Only ever runs when asked.

## `mira remove <id> [--delete-files] [--delete-prefix]`
`DELETE /v1/games/{id}`, with the matching query params if either flag is
given. Without flags, only the `games.toml` entry is forgotten — files are
only ever deleted if you ask for that explicitly, and only if they're
really inside a configured `library_roots`/`prefix_root`.

## `mira steam scan`
`POST /v1/steam/scan` — detects installed Steam apps and adds/updates them
as ordinary games (`GET /v1/games`, `mira list`, `mira show` all work on
one with no special-casing). Prints `added: N  updated: N`. Idempotent:
rerunning it never duplicates an already-detected app.

## `mira library [source]`
`GET /v1/library` — what the account *owns* on each storefront, as opposed
to what Mira tracks (`mira list`). Entitlements aren't stored in
`games.toml`; they're read through live from each source. One line per
title, marked `[installed]` when Mira already tracks it:
```
epic     e8bbb84be35640cda646233152ff3428  [installed]  Brotato
epic     d26da9e047e4440a80781f13a8b7c062               Botanicula
```
`mira library epic` / `mira library steam` / `mira library gog` / `mira
library itch` narrows it to one source. A source that isn't set up
contributes nothing rather than erroring, so an empty listing means
"nothing owned, or nothing configured" — each source's own `mira <source>
status` tells the two apart. Steam needs `steam.web_api_key` +
`steam.steamid64` to report anything here at all, since Steam's on-disk
files only describe games that are already installed. Humble Bundle isn't
part of this listing at all — see `mira humble library`.

## `mira library install <source> <ref>` / `mira library update <source> <ref>`
`POST /v1/library/install` — installs a title the account owns but Mira
doesn't track yet. `<ref>` is the id `mira library` prints (Legendary's
app_name, Steam's appid, GOG's/itch's numeric game id). Returns
immediately; the download runs detached, so watch `mira watch` for
`library.install.finished`. For Epic/GOG/itch this drives the source's own
install tool and then imports and provisions the result; for Steam it
hands off to the Steam client (`steam://install/<appid>`) and the game
appears on the next `mira steam scan`. `update` works for
Epic/GOG/itch — Steam updates its own games.

## `mira epic setup|status|login|logout|import`
Epic Games Store support, via [Legendary](https://github.com/derrod/legendary).
- `setup` — downloads Legendary's latest release binary into
  `~/.config/mira/tools/legendary`. Re-run it to update. Needed once before
  anything else here works.
- `status` — whether Legendary is installed (and from where) and whether
  it's authenticated, in one call. Safe before setup.
- `login` — prints Epic's login URL, then reads back either the
  `authorizationCode` or the whole JSON blob that page shows and pastes it
  through to `legendary auth`. The interactive part lives here rather than
  in `mirad`, which is headless and has no browser.
- `logout` — `legendary auth --delete`.
- `import` — adds already-installed Epic titles as ordinary games, tagged
  `epic`. Prints `added: N  updated: N`. Titles that aren't installed stay
  out of `games.toml` — see `mira library`.

Installing is deliberately not an `epic` subcommand: it's source-generic,
so it lives under `mira library install epic <app_name>`.

## `mira gog setup|status|login|logout|import`
GOG support, via [gogdl](https://github.com/Heroic-Games-Launcher/heroic-gogdl)
(Heroic's own downloader — needs a system `python3`, unlike Legendary's
self-contained binary). Same verb shape as `mira epic`, with one real
difference: gogdl has no "list installed" command of its own, so `import`
doesn't scan the whole system — it re-identifies whatever's already under
`gog.install_root` (default `~/Games/GOG`), which is what `mira library
install gog <id>` itself installs into. A GOG install living somewhere else
isn't picked up.
- `setup` — downloads gogdl's latest release into
  `~/.config/mira/tools/gog/gogdl`.
- `status` — whether gogdl is installed and whether Mira has a stored,
  unexpired token (refreshed transparently if not).
- `login` — prints GOG's login URL, reads back the `code` query param from
  the redirect it shows.
- `logout` — removes Mira's own stored token.
- `import` — see above.

Installing is source-generic: `mira library install gog <id>`.

## `mira itch setup|status|login|logout|import`
itch.io support, via [butlerd](https://itch.io/docs/butler/launcher-integration.html)
— itch's own launcher-integration daemon, the one source here with a tool
built specifically for third-party launchers. `mirad` keeps one `butler
daemon` connection open for its whole run rather than spawning a fresh one
per call; a change to `itch.butler_bin` only takes effect on `mirad`'s next
restart.
- `setup` — downloads butler's latest release into
  `~/.config/mira/tools/itch/` (a zip — butler ships with shared libraries
  that have to stay alongside the binary, not a bare file).
- `status` — whether butler is installed and whether an API key is stored.
- `login` — prompts for an API key from
  [itch.io/user/settings/api-keys](https://itch.io/user/settings/api-keys)
  (no login URL/code flow — itch keys don't expire).
- `logout` — removes Mira's own stored key.
- `import` — adds already-installed itch titles (butlerd's own `Fetch.
  Caves`) as ordinary games, tagged `itch`.

Installing is source-generic: `mira library install itch <id>`.

## `mira humble setup|status|login|library|download`
Humble Bundle support, via [humble-cli](https://github.com/smbl64/humble-cli)
(unofficial). Deliberately its own command family, not folded into `mira
library` — Humble Bundle has no "installed" concept at all, just purchased
bundles of downloadable files, so there's no install/update lifecycle to
plug in there.
- `setup` — downloads humble-cli's latest release.
- `status` — whether humble-cli is installed and authenticated.
- `login <session-key>` — the `_simpleauth_sess` cookie value from a
  logged-in humblebundle.com browser session (documented in humble-cli's
  own README) — no login URL to visit.
- `library` — lists purchased bundles: key, claimed status, name.
- `download <bundle-key> [item-numbers]` — downloads items from one bundle
  (optionally narrowed, humble-cli's own "1,3,5-7" syntax) into
  `<humble.download_root>/<bundle_key>/`. Returns immediately; watch `mira
  watch` for `humble.download.finished`. Not auto-added as a game — a
  downloaded item is an arbitrary archive/installer, not a provisioned
  prefix; add it manually once it's landed (`mira add`, same as any other
  manually-acquired game).

## `mira amazon setup|status|login|logout|import`
Amazon Games / Prime Gaming, via [nile](https://github.com/imLinguin/nile).
- `setup` — downloads nile's latest release.
- `status` — whether nile is installed and logged in.
- `login` — prints an Amazon login URL, then asks for the amazon.com URL
  the browser ends on.
- `import` — adds games nile has installed.

Install with `mira library install amazon <product id>`.

## `mira launcher list|install|import|open`
Battle.net (`battlenet`), Ubisoft Connect (`ubisoft`) and the EA app (`ea`),
each installed into its own prefix. See [the API](api.md#store-launchers).
- `list` — each launcher and whether it's installed.
- `install <id>` — sets up the prefix and installs the launcher, then
  imports its games. Battle.net's installer is shown to click through.
- `import <id>` — imports games installed through the launcher since.
- `open <id> [--launch REF | --install REF]` — opens the launcher, or asks
  it to launch or install a game by store id.

Imported games launch with `mira launch` like any other.

## `mira metadata <id> [--refresh]`
`GET /v1/games/{id}/metadata` — prints the cached cover-art/store-info JSON
(description, genres, release date, developers/publishers, price, Steam
review summary, ProtonDB tier — see `docs/api.md`'s Metadata section for
which fields come from where). `--refresh` instead calls `POST
.../metadata/refresh` and returns immediately; watch `mira watch` for
`game.metadata_ready`/`.metadata_failed`. Cover art itself has no CLI
command — it's a binary image, only reachable via `GET
/v1/games/{id}/artwork` directly.

## `mira runners`
`GET /v1/runners` — every installed Proton/Wine build, one per line:
```
proton:GE-Proton11-7                    /home/x/.steam/steam/compatibilitytools.d/GE-Proton11-7-x86_64
```

## `mira runners catalog [--kind proton|wine]`
`GET /v1/runners/catalog` — what's *available to download*, not what's
installed. Hits the GitHub API live, so this is the one command here with
real network latency. Defaults to `--kind proton`.

## `mira runners download --kind proton|wine --tag TAG`
`POST /v1/runners/download` — downloads and installs a build named in the
catalog above (checksum-verified against the release's own `.sha512sum`
first). Runs detached; the command returns immediately and says to watch
`mira watch` for `runners.download.finished`/`.failed`.

## `mira runners schema <kind>`
`GET /v1/runners/{kind}/schema` — what `game.runner_config` accepts for
that kind (e.g. `gameid` for `proton`). `[]` for a kind with no fields.

## `mira runners remove <kind:name>`
`DELETE /v1/runners/{kind}:{name}` — the other half of `download`:
uninstalls a build. Refuses anything not really inside
`runner_search_paths`/`wine_search_paths` — the system wine, or a
hand-edited path, can't be removed this way.

## `mira config get|set|list|reset`
- `get <key>` — one value from `GET /v1/config` (dotted key, e.g.
  `scan.debounce_ms`).
- `set <key> <value>` — `PATCH /v1/config` with that one key. Value is
  parsed as JSON first, same fallback-to-string rule as `--override` above.
- `list` — every setting from `GET /v1/config/schema`: key, type, scope
  (`global` or `per_game`), one-line doc. This is the whole settings reference; there's no need to
  cross-reference `docs/api.md`'s schema section by hand.
- `reset [key]` — `POST /v1/config/reset`, one key or everything.

`get`/`set` work on frontend settings too, unvalidated: `mira config set
frontend.theme dark` / `mira config get frontend.theme` reach into the
opaque `frontend` table `GET`/`PATCH /v1/config` already carry (see
`docs/api.md`'s Settings section) and land in `frontend.toml`, not
`settings.toml` — arbitrary nesting works the same way `--override` does
(`frontend.window.width 1200`). `list` won't show these keys, since it only
enumerates the schema-registered ones — the frontend's settings shape is
its own, not backend-validated.

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

`mira` has no `runners refresh` or per-runner schema command, because
`GET /v1/runners/{kind}/schema` and `POST /v1/runners/refresh` don't exist
yet either (`docs/api.md` marks them **planned** — the latter isn't needed
today since `GET /v1/runners` already rediscovers on every call). There's
also no equivalent of the old-plan `resetup` — a game stuck at
`setting_up` is retried automatically on the next scan, and a
`needs_install` game uses `mira install` (or `mira run` + `mira
finish-install`) instead (see above), which cover the same need more precisely than a single
"re-run everything from scratch" command would.
