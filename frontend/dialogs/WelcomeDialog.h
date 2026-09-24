#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

namespace mira_gui {

// First launch: confirms (or changes) the games folder Mira takes over, then
// offers to bring in games from Steam, Lutris and the stores.
class WelcomeDialog : public QDialog {
  Q_OBJECT

public:
  // `folder` is the current first library root, e.g. "~/Games".
  WelcomeDialog(const QString& folder, QWidget* parent = nullptr);

signals:
  // The games folder was set; what's already in it still needs a scan.
  void GamesFolderChanged();
  // Steam or Lutris added games.
  void LibraryChanged();
  // The dialog has closed so that source's page can open.
  void OpenSourceRequested(QString id);

private:
  QWidget* BuildFolderPage();
  QWidget* BuildImportPage();
  void UpdateFolderNote();
  void UseFolder();

  QStackedWidget* pages_ = nullptr;
  QLineEdit* folder_ = nullptr;
  QLabel* folder_note_ = nullptr;
  QLabel* folder_error_ = nullptr;
  QPushButton* use_folder_ = nullptr;
};

}  // namespace mira_gui
