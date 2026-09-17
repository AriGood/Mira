#include "GameActions.h"

#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>
#include <QWidget>

#include <utility>

#include "../client/MiradClient.h"
#include "../dialogs/DeleteGameDialog.h"
#include "../dialogs/RunInPrefixDialog.h"

namespace mira_gui::actions {

void Launch(QWidget* parent, const std::string& id, std::function<void()> on_launched) {
  MiradClient::LaunchGameAsync(parent, id, [parent, on_launched](LaunchResult result) {
    if (!result.ok) {
      QMessageBox::warning(parent, "Launch failed", QString::fromStdString(result.error));
      return;
    }
    if (on_launched) on_launched();
  });
}

void Stop(QWidget* parent, const std::string& id) {
  MiradClient::StopGameAsync(parent, id, [parent](StopResult result) {
    if (!result.ok) {
      QMessageBox::warning(parent, "Stop failed", QString::fromStdString(result.error));
    }
  });
}

void Delete(QWidget* parent, const std::string& id, const QString& name,
            std::function<void()> on_deleted) {
  // The paths have to come from GET /v1/games/{id}: the list endpoint's
  // summary carries neither install_path nor data_dir, and a prompt offering
  // to delete a directory it can't name is not one anyone can answer. A
  // failed fetch still offers the plain remove, with both options disabled.
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
            QMessageBox::warning(parent, "Remove failed",
                                 QString("Failed to remove \"%1\": %2")
                                     .arg(name, QString::fromStdString(result.error)));
            return;
          }
          if (on_deleted) on_deleted();
        });
  });
}

void RunInPrefix(QWidget* parent, const std::string& id) {
  MiradClient::GetGameAsync(parent, id, [parent, id](GameDetailResult result) {
    if (!result.ok) {
      QMessageBox::warning(parent, "Run failed", QString::fromStdString(result.error));
      return;
    }
    RunInPrefixDialog dialog(id, result.game, parent);
    dialog.exec();
  });
}

void FinishInstall(QWidget* parent, const std::string& id, std::function<void()> on_finished) {
  MiradClient::FinishInstallAsync(parent, id, [parent, on_finished](FinishInstallResult result) {
    if (!result.ok) {
      QMessageBox::warning(parent, "Could not mark as installed",
                           QString("%1\n\nSet the game's executable to whatever the installer "
                                   "produced first, then try again.")
                               .arg(QString::fromStdString(result.error)));
      return;
    }
    if (on_finished) on_finished();
  });
}

void OpenInstallFolder(QWidget* parent, const std::string& id) {
  MiradClient::GetGameAsync(parent, id, [parent](GameDetailResult result) {
    if (!result.ok || result.game.install_path.empty()) {
      QMessageBox::warning(parent, "Open folder failed",
                           result.ok ? QString("This game has no install path.")
                                     : QString::fromStdString(result.error));
      return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(result.game.install_path)));
  });
}

}  // namespace mira_gui::actions
