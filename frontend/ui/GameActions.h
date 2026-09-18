#pragma once

#include <QString>

#include <functional>
#include <string>

class QWidget;

// The things both library views do to a game, with the prompts and error
// reporting that go with them.
//
// The grid and the table used to carry byte-identical copies of launch,
// stop and delete, differing only in what they did afterwards. That
// "afterwards" is the `on_done` callback here; everything before it now
// exists once.
namespace mira_gui::actions {

// POST /v1/games/{id}/launch. `on_launched(tracked)` runs only on success.
// `tracked` is false when mirad handed the game to Steam rather than
// spawning it — no game.state event is coming, so a caller must not record
// it as running.
void Launch(QWidget* parent, const std::string& id, std::function<void(bool tracked)> on_launched);

// POST /v1/games/{id}/stop. Nothing to do on success: the `game.state` event
// that follows is what actually updates the view.
void Stop(QWidget* parent, const std::string& id);

// Asks (see DeleteGameDialog, which offers deleting the files and the
// prefix too), then DELETEs. `on_deleted` runs only if something was removed.
void Delete(QWidget* parent, const std::string& id, const QString& name,
            std::function<void()> on_deleted);

// Runs an executable inside the game's own prefix, asking which one first.
// Needs the full record for its candidate list, so it fetches before
// prompting.
void RunInPrefix(QWidget* parent, const std::string& id);

// Flips a needs_install game to ready. 409s while exe_path is still empty,
// the normal case until the user points it at the installed program — so
// the failure message matters more here than elsewhere.
void FinishInstall(QWidget* parent, const std::string& id, std::function<void()> on_finished);

// Opens the game's install_path in the desktop's file manager. Needs a
// detail fetch first — install_path isn't in the list summary.
void OpenInstallFolder(QWidget* parent, const std::string& id);

}  // namespace mira_gui::actions
