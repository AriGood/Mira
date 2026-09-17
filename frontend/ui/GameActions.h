#pragma once

#include <QString>

#include <functional>
#include <string>

class QWidget;

// The things both library views do to a game, with the prompts and error
// reporting that go with them.
//
// The grid and the table used to carry byte-identical copies of launch,
// stop and delete — same request, same confirmation, same QMessageBox on
// failure — differing only in what they did afterwards. That "afterwards" is
// the `on_done` callback here; everything before it is the same in both and
// now exists once.
namespace mira_gui::actions {

// POST /v1/games/{id}/launch. `on_launched` runs only on success, on the
// main thread — a failed launch reports itself and does nothing else.
void Launch(QWidget* parent, const std::string& id, std::function<void()> on_launched);

// POST /v1/games/{id}/stop. Nothing to do on success: the `game.state` event
// that follows is what actually updates the view.
void Stop(QWidget* parent, const std::string& id);

// Asks (see DeleteGameDialog, which offers deleting the files and the prefix
// too), then DELETEs. `on_deleted` runs only if something was removed.
void Delete(QWidget* parent, const std::string& id, const QString& name,
            std::function<void()> on_deleted);

// Runs an executable inside the game's own prefix
// (POST /v1/games/{id}/run), asking which one first. Needs the full record
// for its candidate list, so it fetches before prompting.
void RunInPrefix(QWidget* parent, const std::string& id);

// Flips a needs_install game to ready (POST /v1/games/{id}/finish-install).
// 409s while exe_path is still empty, which is the normal case right up
// until the user points it at whatever the installer produced — so the
// failure message matters more here than elsewhere.
void FinishInstall(QWidget* parent, const std::string& id, std::function<void()> on_finished);

// Opens the game's install_path in the desktop's file manager. Needs a
// GET /v1/games/{id} first — install_path isn't in the list summary.
void OpenInstallFolder(QWidget* parent, const std::string& id);

}  // namespace mira_gui::actions
