# Mira

A self-hosted Linux game launcher: one library for native games and
Windows games run through Wine/Proton, with Steam games folded in
alongside everything else.

Drop a game folder into a watched directory and Mira detects it, works out
how to run it, and sets it up — no manual "add game" wizard.

This repository is the **backend** (`mirad`), a **CLI** (`mira`), and a
**Qt frontend** (`mira-gui`). See
[`docs/architecture.md`](docs/architecture.md) for how the three relate,
and [`docs/frontend.md`](docs/frontend.md) for the frontend specifically —
short version: `mira` and `mira-gui` are both plain REST clients over a Unix
socket, and neither can do anything `mirad` doesn't expose through
[`docs/api.md`](docs/api.md).

**Status:** early, but the core loop works end to end: detection, Wine/Proton
prefix provisioning, launching, crash/playtime tracking, Steam game
detection and process tracking, desktop-menu integration, cover-art/store
metadata fetching, winetricks, and runner (Proton/Wine build) downloading
are all built and tested. See `docs/architecture.md` for what's still
planned.

## Building

```sh
git clone <this repo>
cd mira
cmake --preset dev
cmake --build build/dev
```

Needs a C++23 compiler, CMake ≥ 3.20, Ninja, and optionally Qt6 (≥ 6.5,
`Widgets`) for `mira-gui`. Nothing else — dependencies are vendored,
header-only. Binaries land in `build/dev/`. Other presets (`release`,
`asan`, `tsan`), the frontend-only build (`cmake -S frontend -B
frontend/build`), and the AppImage/`run-gui` targets are in
`CMakeLists.txt`/`CMakePresets.json`.

## Running it

```sh
build/dev/mirad &
build/dev/mira status
build/dev/mira list
build/dev/mira watch
```

or `build/frontend/mira-gui`. `docs/cli.md` covers every `mira` command;
`docs/architecture.md` covers the three ways to actually run `mirad`
(foreground, systemd, frontend-supervised) and the systemd path:

```sh
systemctl --user enable --now mirad.service
```

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
