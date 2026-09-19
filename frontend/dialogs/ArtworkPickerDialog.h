#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include "../client/EventStream.h"
#include "../client/Types.h"

class QLabel;
class QListWidget;
class QPushButton;

namespace mira_gui {

// Browses SteamGridDB's own candidate list for one art slot ("cover" or
// "hero"; art_candidates in GET .../metadata), applying each one as it's
// picked rather than showing a thumbnail grid up front — the candidates' own
// images live on SteamGridDB's CDN, and the frontend has no HTTP client for
// the open internet, only mirad's socket. Picking a row from the list is the
// preview: mirad fetches and caches it, and this dialog shows the same real,
// cached image the grid tile (or the details panel's banner) will.
class ArtworkPickerDialog : public QDialog {
  Q_OBJECT

public:
  ArtworkPickerDialog(std::string game_id, std::string slot, QWidget* parent = nullptr);

private:
  void Load();
  void Select(int index);
  void RefreshPreview();
  void SetBusy(bool busy);
  void FetchFromSteamGridDb();
  void HandleEvent(const std::string& type, const std::string& data);

  std::string id_;
  std::string slot_;  // "cover" or "hero"
  std::vector<ArtCandidate> candidates_;
  int current_ = -1;    // index into candidates_ currently applied
  int pending_ = -1;    // index a Select() is waiting to hear back on
  bool busy_ = false;
  // True between FetchFromSteamGridDb() and the matching game.metadata_ready/
  // .metadata_failed — separate from `busy_`, which SetBusy also toggles for
  // an in-flight Select().
  bool fetching_ = false;

  QListWidget* list_ = nullptr;
  QLabel* preview_ = nullptr;
  QLabel* status_ = nullptr;
  // Shown only when this slot has no cached candidates yet — lets a game
  // whose metadata predates a SteamGridDB key (or predates Steam-owned games
  // getting candidates at all) get them without leaving the dialog.
  QPushButton* fetch_button_ = nullptr;

  EventStream event_stream_;
};

}  // namespace mira_gui
