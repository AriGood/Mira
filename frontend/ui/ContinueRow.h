#pragma once

#include <QPoint>
#include <QString>
#include <QWidget>

#include <set>
#include <string>
#include <vector>

#include "../client/Types.h"

class QHBoxLayout;

namespace mira_gui {

class ArtworkStore;

// Large cards above the library grid: running games first, then the most
// recently played.
class ContinueRow : public QWidget {
  Q_OBJECT

public:
  ContinueRow(ArtworkStore* artwork, QWidget* parent = nullptr);

  void SetGames(const std::vector<const GameSummary*>& games);
  bool Shows(const std::string& id) const { return shown_.contains(id); }

signals:
  void PlayToggled(QString id);
  void MenuRequested(QString id, QPoint global_pos);

private:
  QWidget* MakeCard(const GameSummary& game, bool running);

  ArtworkStore* artwork_ = nullptr;
  QHBoxLayout* cards_ = nullptr;
  std::set<std::string> shown_;
};

}  // namespace mira_gui
