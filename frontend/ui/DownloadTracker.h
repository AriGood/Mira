#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <string>
#include <vector>

class QTimer;

namespace mira_gui {

// Everything mirad is installing or downloading, built from the event
// stream: game installers, store installs and updates, Humble downloads,
// launcher installs, store helper downloads and runner downloads. mirad
// replays its recent events to a new stream, so this also knows about work
// started before the GUI was.
//
// Only game installers report how far along they are (bytes written);
// the rest are only started, finished or failed.
class DownloadTracker : public QObject {
  Q_OBJECT

public:
  enum class Kind { Game, Title, Humble, Launcher, Tool, Runner };
  enum class State { Running, Finished, Failed };

  struct Entry {
    QString key;  // see KeyFor
    Kind kind = Kind::Game;
    QString source;  // a source id; empty for Game and Runner
    QString ref;     // game id, title ref, bundle key, launcher id or runner tag
    bool update = false;
    State state = State::Running;
    qint64 bytes = 0;  // Game only
    QString error;
    QDateTime changed;
  };

  explicit DownloadTracker(QObject* parent = nullptr);

  static QString KeyFor(Kind kind, const QString& source, const QString& ref);

  // Returns whether the event was one of ours.
  bool HandleEvent(const std::string& type, const std::string& data);

  // Newest first.
  const std::vector<Entry>& Entries() const { return entries_; }
  const Entry* Find(const QString& key) const;
  int RunningCount() const;
  void ClearFinished();

  // A display name for an entry. Store titles use NoteTitle's names, then
  // the game they became, then their ref.
  QString NameFor(const Entry& entry) const;
  void NoteTitle(const QString& source, const QString& ref, const QString& title);

  // The installed game an entry is, or became; empty if none.
  static QString GameIdFor(const Entry& entry);

  // Set by the owner: a tracked game's name, or empty.
  std::function<QString(const std::string& id)> game_name;
  // Set by the owner: a source's display name.
  std::function<QString(const QString& source)> source_name;

signals:
  // One entry was added or changed; empty when several were (ClearFinished).
  void Changed(const QString& key);

private:
  Entry& Upsert(Kind kind, const QString& source, const QString& ref);
  void Poll();
  void ResolveNames(const QString& source);

  std::vector<Entry> entries_;
  QHash<QString, QString> titles_;  // "<source>:<ref>" -> title
  QSet<QString> asked_sources_;     // title lists already fetched to name entries
  QTimer* poll_ = nullptr;
};

}  // namespace mira_gui
