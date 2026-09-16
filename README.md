# Mira

A Linux game launcher for native games and Windows games run through
Wine/Proton. Drop a game folder into a watched directory and Mira detects
it, works out how to run it, and sets it up — no manual "add game" wizard.

This repository is the **backend** (`mirad`), a **CLI** (`mira`), and a
currently-blank **Qt frontend skeleton** (`mira-gui`). See
[`docs/architecture.md`](docs/architecture.md) for how the three relate —
short version: `mira` and `mira-gui` are both plain REST clients over a Unix
socket, and neither can do anything `mirad` doesn't expose through
[`docs/api.md`](docs/api.md).

**Status:** early. Settings, the game library, the REST API, and automatic
detection/scanning/watching are built and tested; provisioning a Wine/Proton
prefix and launching a game are not — see the "Planned, not yet built"
section of `docs/architecture.md`.

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

## Running it

```sh
build/dev/mirad --foreground
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

`docs/architecture.md` documents the three supported ways to actually run
`mirad` day-to-day (a systemd user service, spawned and supervised by the
frontend, or one-shot with nothing persistent) — `--foreground` above is the
right way to run it while developing, not the recommended end-user path.

## Where things live

All user data — settings and the game library — lives in one directory,
`~/.config/mira/` (`$XDG_CONFIG_HOME/mira`), as two TOML files:
`settings.toml` and `games.toml`. Back that directory up and you have the
whole app's state; delete it and Mira starts fresh. See
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
