# Mira

A Linux game launcher for native games and Windows games run through
Wine/Proton. Drop a game folder into a watched directory and Mira detects
it, works out how to run it, and sets it up — no manual "add game" wizard.

This repository is the **backend** (`mirad`), a **CLI** (`mira`), and a
**Qt frontend** (`mira-gui`). See
[`docs/architecture.md`](docs/architecture.md) for how the three relate,
and [`docs/frontend.md`](docs/frontend.md) for the frontend specifically —
short version: `mira` and `mira-gui` are both plain REST clients over a Unix
socket, and neither can do anything `mirad` doesn't expose through
[`docs/api.md`](docs/api.md).

**Status:** early, but the core loop works end to end: detection, Wine/Proton
prefix provisioning, launching, crash/playtime tracking, Steam game
detection and launching, and desktop-menu integration are all built and
tested. See `docs/architecture.md` for what's still planned (runner-version
downloading is in progress; winetricks-equivalent tooling is deliberately
deferred).

## Building

Everything is header-only except the C++ standard library itself — no
package manager step, no network access needed to build.

```sh
git clone <this repo>
cd mira
cmake --preset dev          # or: release
cmake --build build/dev
```

Requirements: a C++23 compiler (developed against Clang 22; GCC 13+ should
work), CMake ≥ 3.20, Ninja. Qt6 (≥ 6.5, `Widgets`) is optional — if it isn't
found, `mira-gui` is silently skipped and `mirad`/`mira` build exactly the
same either way. Nothing else is required; `third_party/` vendors
[cpp-httplib](https://github.com/yhirose/cpp-httplib),
[nlohmann/json](https://github.com/nlohmann/json),
[toml++](https://github.com/marzer/tomlplusplus), and
[doctest](https://github.com/doctest/doctest) as single headers.

Binaries land in `build/dev/`: `mirad`, `mira`, `mira_tests`, and
`frontend/mira-gui` if Qt6 was found.

### Presets

| Preset    | What it's for |
|-----------|----------------|
| `dev`     | debug build, `-Werror` on |
| `release` | optimized, `RelWithDebInfo` |
| `asan`    | AddressSanitizer + UBSan |
| `tsan`    | ThreadSanitizer — the one that matters most here; the daemon is multi-threaded and shares state (settings, the game store, the event bus) across those threads |

```sh
cmake --preset tsan && cmake --build build/tsan
```

### Building just the frontend

`frontend/` also configures standalone, for frontend-only iteration without
touching the backend build at all:

```sh
cmake -S frontend -B frontend/build
cmake --build frontend/build
```

## Running it

```sh
build/dev/mirad
```

runs the daemon in the foreground, logging to stderr, listening on
`$XDG_RUNTIME_DIR/mira/mirad.sock`. Stop it with Ctrl-C.

In another terminal:

```sh
build/dev/mira status                        # is it reachable?
build/dev/mira config list                    # every setting, with docs
build/dev/mira config get scan.debounce_ms
build/dev/mira config set scan.debounce_ms 5000
build/dev/mira scan                           # scan every library root now
build/dev/mira list                           # games in the library
build/dev/mira watch                          # tail the live event stream
```

Every command is documented in [`docs/cli.md`](docs/cli.md). Drop a game
folder into any of your configured `library_roots` (`~/Games` by default)
while `mirad` is running and it shows up in `mira list` on its own, no
`scan` needed — that's `library::Watcher`, not a poll (see
`docs/architecture.md`).

or use
```sh
build/frontend/mira-gui
```
to launch the frontend

`docs/architecture.md` documents the three supported ways to actually run
`mirad` day-to-day (a systemd user service, spawned and supervised by the
frontend, or one-shot with nothing persistent) — running it directly is the
right way to run it while developing, not the recommended end-user path.

### Running the frontend

`mira-gui` opens on a cover-art library grid: one click selects a game and
shows its details, a second launches it, right-click opens its menu. The
older table view — every field of every game at once, which is what you want
when auditing a fresh scan — is still there as `mira-gui --classic`, or via
the grid's View menu. To build and launch the frontend in one step during
development:

```sh
cmake --build build/dev --target run-gui
```

This is a plain convenience target (build `mira-gui`, then exec it) and
isn't part of the install story. It only exists if Qt6 was found at
configure time — see the "Building" section above.

### Building an AppImage

```sh
cmake --build build/dev --target appimage
```

Produces `build/dev/Mira-x86_64.AppImage`, bundling `mirad`, `mira`, and
`mira-gui` — the non-Arch install path the AUR `PKGBUILD` doesn't cover.
Needs Qt6 (same as `run-gui` above) and `curl` on `PATH`; the packaging
tools themselves (`linuxdeploy`, `linuxdeploy-plugin-qt`) are downloaded
into `build/dev/appimage-tools/` on first use and cached there — nothing
is installed system-wide. See `cmake/AppImage.cmake` for the two real
environment-specific workarounds it applies (a filtered Qt plugins
directory, and `NO_STRIP=1`) and why they're there.

## Where things live

All user data — settings and the game library — lives in one directory,
`~/.config/mira/` (`$XDG_CONFIG_HOME/mira`), as three TOML files:
`settings.toml` (backend config, schema-validated), `games.toml` (the
library), and `frontend.toml` (the frontend's own settings — opaque to the
backend, never validated against the schema). Back that directory up and
you have the whole app's state; delete it and Mira starts fresh. See
`docs/architecture.md` for why TOML rather than a database.

## Tests

```sh
cmake --build build/dev --target mira_tests
build/dev/mira_tests
```

or `ctest --test-dir build/dev`. Run the same suite under TSan before
trusting a change that touches anything shared across threads
(`Config`, `GameStore`, `EventBus`):

```sh
cmake --build build/tsan --target mira_tests
build/tsan/mira_tests
```

## Documentation

- [`docs/architecture.md`](docs/architecture.md) — how the pieces fit
  together, the backend/frontend split, storage design, the three ways to
  run the daemon, idle-cost rules, and what's planned but not built.
- [`docs/api.md`](docs/api.md) — the full REST surface, endpoint by
  endpoint, with what's implemented versus planned.
- [`docs/cli.md`](docs/cli.md) — every `mira` command, what it does, and
  which API call it makes.

## License

[GPLv3](LICENSE).
