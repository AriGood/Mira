#pragma once

#include <QString>
#include <QWidget>

#include <string>

#include "../client/Types.h"

class QLabel;
class QPushButton;
class QStackedWidget;

namespace mira_gui {

class ArtworkStore;

// The side panel a selected game fills in: cover, name, status, the
// play/stop button, and its metadata.
//
// Split out of LibraryWindow because it is the one part of that window with
// state of its own to keep straight — which game it last fetched a path for,
// so that rebuilding the grid on every keystroke doesn't re-issue the
// request or flicker the field. It reports what the user asked for and lets
// the window decide what that means.
class GameDetailsPanel : public QWidget {
  Q_OBJECT

public:
  explicit GameDetailsPanel(QWidget* parent = nullptr);

  // The store the panel's cover comes from. Shared with the grid so the
  // same image is fetched, decoded and cached once for both. Without one
  // the panel still works and shows placeholders.
  void SetArtworkStore(ArtworkStore* store);

  void ShowGame(const GameSummary& game, bool running);
  void Clear();

  // Re-draw the cover for the game currently on screen. For when artwork
  // arrives after the panel was filled in.
  void RefreshCover(const GameSummary& game);

signals:
  void PlayRequested(const QString& id);
  void StopRequested(const QString& id);
  void EditRequested(const QString& id);
  void MetadataRefreshRequested(const QString& id);

private:
  QStackedWidget* stack_ = nullptr;
  QLabel* cover_ = nullptr;
  QLabel* name_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* platform_ = nullptr;
  QLabel* runner_ = nullptr;
  QLabel* confidence_ = nullptr;
  QLabel* last_played_ = nullptr;
  QLabel* playtime_ = nullptr;
  QLabel* path_ = nullptr;
  QLabel* error_ = nullptr;
  QPushButton* play_ = nullptr;

  ArtworkStore* artwork_ = nullptr;
  std::string game_id_;
  bool running_ = false;
};

}  // namespace mira_gui
