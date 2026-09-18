#include "GameActions.h"

#include <QDesktopServices>
#include <QUrl>
#include <QWidget>

#include <utility>

#include "../client/MiradClient.h"
#include "../dialogs/DeleteGameDialog.h"
#include "../dialogs/RunInPrefixDialog.h"
#include "Notify.h"

namespace mira_gui::actions {

void Launch(QWidget* parent, const std::string& id, std::function<void(bool tracked)> on_launched) {
  MiradClient::LaunchGameAsync(parent, id, [parent, on_launched](LaunchResult result) {
    if (!result.ok) {
      notify::Failed(parent, "Could not launch the game.", QString::fromStdString(result.error));
      return;
    }
    // Untracked (handed to Steam) is worth a toast, but mirad already sends
    // one as a `notification` event alongside game.launched.
    if (on_launched) on_launched(result.tracked);
  });
}

void Stop(QWidget* parent, const std::string& id) {
  MiradClient::StopGameAsync(parent, id, [parent](StopResult result) {
    if (!result.ok) {
      notify::Failed(parent, "Could not stop the game.", QString::fromStdString(result.error));
    }
  });
}

void Delete(QWidget* parent, const std::string& id, const QString& name,
            std::function<void()> on_deleted) {
  // Paths come from a detail fetch — the list summary carries neither
  // install_path nor data_dir. A failed fetch still offers the plain
  // remove, with both options disabled.
  MiradClient::GetGameAsync(parent, id, [parent, id, name, on_deleted](GameDetailResult detail) {
    const QString install_path =
        detail.ok ? QString::fromStdString(detail.game.install_path) : QString();
    const QString data_dir = detail.ok ? QString::fromStdString(detail.game.data_dir) : QString();

    const DeleteChoice choice = AskDeleteGame(parent, name, install_path, data_dir);
    if (!choice.confirmed) return;

    MiradClient::DeleteGameAsync(
        parent, id, choice.delete_files, choice.delete_prefix,
        [parent, name, on_deleted](DeleteResult result) {
          if (!result.ok) {
            notify::Failed(parent, QString("Could not remove \"%1\".").arg(name),
                           QString::fromStdString(result.error));
            return;
          }
          if (on_deleted) on_deleted();
        });
  });
}

void RunInPrefix(QWidget* parent, const std::string& id) {
  MiradClient::GetGameAsync(parent, id, [parent, id](GameDetailResult result) {
    if (!result.ok) {
      notify::Failed(parent, "Could not open this game's prefix.",
                     QString::fromStdString(result.error));
      return;
    }
    RunInPrefixDialog dialog(id, result.game, parent);
    dialog.exec();
  });
}

void FinishInstall(QWidget* parent, const std::string& id, std::function<void()> on_finished) {
  MiradClient::FinishInstallAsync(parent, id, [parent, on_finished](FinishInstallResult result) {
    if (!result.ok) {
      notify::FailedWithHint(parent, "Could not mark this game as installed.",
                             QString::fromStdString(result.error),
                             "Set the game's executable to whatever the installer produced "
                             "first, then try again.");
      return;
    }
    if (on_finished) on_finished();
  });
}

void OpenInstallFolder(QWidget* parent, const std::string& id) {
  MiradClient::GetGameAsync(parent, id, [parent](GameDetailResult result) {
    if (!result.ok || result.game.install_path.empty()) {
      notify::Failed(parent, "Could not open the install folder.",
                     result.ok ? QString("This game has no install path.")
                               : QString::fromStdString(result.error));
      return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(result.game.install_path)));
  });
}

}  // namespace mira_gui::actions
