#include "GameActions.h"

#include <QDesktopServices>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include <memory>
#include <utility>

#include "../client/MiradClient.h"
#include "../dialogs/DeleteGameDialog.h"
#include "../dialogs/LogViewerDialog.h"
#include "../dialogs/RunInPrefixDialog.h"
#include "../dialogs/WinetricksDialog.h"
#include "Notify.h"

namespace mira_gui::actions {

void Launch(QWidget* parent, const std::string& id, std::function<void(bool tracked)> on_launched) {
  MiradClient::LaunchGameAsync(parent, id, [parent, on_launched](LaunchResult result) {
    if (!result.ok) {
      notify::Failed(parent, "Could not launch the game.", QString::fromStdString(result.error));
      return;
    }
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
    const QString source = detail.ok ? QString::fromStdString(detail.game.source) : QString();

    const DeleteChoice choice = AskDeleteGame(parent, name, install_path, data_dir, source);
    if (!choice.confirmed) return;

    MiradClient::DeleteGameAsync(
        parent, id, choice.delete_files, choice.delete_prefix, choice.delete_metadata,
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

void RunInPrefix(QWidget* parent, const std::string& id, const std::string& install_path,
                 const QString& name) {
  RunInPrefixDialog dialog(id, install_path, name, parent);
  dialog.exec();
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

void OpenInstallFolder(QWidget* parent, const std::string& install_path) {
  // install_path is already on GameSummary, so a fetch here would only ever
  // reproduce what the caller already has.
  if (install_path.empty()) {
    notify::Failed(parent, "Could not open the install folder.", "This game has no install path.");
    return;
  }
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(install_path)));
}

void ViewLog(QWidget* parent, const std::string& id, const QString& name) {
  LogViewerDialog dialog(id, name, parent);
  dialog.exec();
}

void RunWinetricks(QWidget* parent, const std::string& id, const QString& name) {
  MiradClient::GetGameAsync(parent, id, [parent, id, name](GameDetailResult result) {
    if (!result.ok) {
      notify::Failed(parent, "Could not run winetricks.", QString::fromStdString(result.error));
      return;
    }
    if (result.game.data_dir.empty()) {
      notify::FailedWithHint(parent, "Can't run winetricks yet.",
                             "This game has no Wine/Proton prefix provisioned.",
                             "Run something in its prefix first (\"Run in prefix…\"), which "
                             "provisions one on demand.");
      return;
    }
    WinetricksDialog dialog(id, name, parent);
    dialog.exec();
  });
}

void ToggleDesktopEntry(QWidget* parent, const std::string& id, bool currently_enabled) {
  const GameConfigEdit edit{"desktop_entries.enabled", "a boolean",
                            currently_enabled ? "false" : "true", false};
  MiradClient::PatchGameConfigAsync(
      parent, id, {edit}, [parent, currently_enabled](PatchGameConfigResult result) {
        if (!result.ok) {
          notify::Failed(parent, "Could not update the desktop entry.",
                         QString::fromStdString(result.error));
          return;
        }
        // The only feedback there is — nothing in Mira's own window changes.
        notify::Notice(parent, currently_enabled ? "Removed from the application menu."
                                                 : "Added to the application menu.");
      });
}

void BatchDelete(QWidget* parent, const std::vector<std::pair<std::string, QString>>& games,
                 std::function<void()> on_done) {
  if (games.empty()) return;
  const DeleteChoice choice = AskDeleteGames(parent, static_cast<int>(games.size()));
  if (!choice.confirmed) return;

  auto remaining = std::make_shared<int>(static_cast<int>(games.size()));
  auto failed = std::make_shared<QStringList>();

  for (const auto& [id, name] : games) {
    MiradClient::GetGameAsync(parent, id, [parent, id, name, choice, remaining, failed,
                                           on_done](GameDetailResult detail) {
      const bool linked_only = detail.ok && detail.game.source == "desktop-entry";
      MiradClient::DeleteGameAsync(
          parent, id, choice.delete_files && !linked_only, choice.delete_prefix && !linked_only,
          choice.delete_metadata, [parent, name, remaining, failed, on_done](DeleteResult result) {
            if (!result.ok) *failed << name;
            if (--*remaining > 0) return;
            // Success needs no notice: the games leave the grid.
            if (!failed->isEmpty()) {
              notify::Failed(parent, QString("Could not remove %1 game(s).").arg(failed->size()),
                             failed->join(", "));
            }
            if (on_done) on_done();
          });
    });
  }
}

void BatchSetDesktopEntry(QWidget* parent, const std::vector<std::string>& ids, bool enabled) {
  if (ids.empty()) return;
  const GameConfigEdit edit{"desktop_entries.enabled", "a boolean", enabled ? "true" : "false",
                            false};
  auto remaining = std::make_shared<int>(static_cast<int>(ids.size()));
  auto failures = std::make_shared<int>(0);

  for (const std::string& id : ids) {
    MiradClient::PatchGameConfigAsync(
        parent, id, {edit}, [parent, enabled, remaining, failures](PatchGameConfigResult result) {
          if (!result.ok) ++*failures;
          if (--*remaining > 0) return;
          if (*failures > 0) {
            notify::Failed(parent, QString("Could not update %1 game(s).").arg(*failures),
                           "See each game's own Advanced settings to retry.");
          } else {
            notify::Notice(parent, enabled ? "Added to the application menu."
                                           : "Removed from the application menu.");
          }
        });
  }
}

}  // namespace mira_gui::actions
