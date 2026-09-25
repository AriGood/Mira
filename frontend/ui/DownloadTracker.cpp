#include "DownloadTracker.h"

#include <QTimer>

#include <algorithm>

#include "../client/MiradClient.h"

namespace mira_gui {
namespace {

bool ToState(const std::string& state, DownloadTracker::State* out) {
  if (state == "started") {
    *out = DownloadTracker::State::Running;
  } else if (state == "finished") {
    *out = DownloadTracker::State::Finished;
  } else if (state == "failed") {
    *out = DownloadTracker::State::Failed;
  } else {
    return false;
  }
  return true;
}

// The helper each store's setup downloads.
QString ToolName(const QString& source) {
  if (source == "epic") return "Legendary";
  if (source == "gog") return "gogdl";
  if (source == "itch") return "butler";
  if (source == "humble") return "humble-cli";
  if (source == "amazon") return "nile";
  if (source == "umu") return "umu-launcher";
  return source;
}

}  // namespace

DownloadTracker::DownloadTracker(QObject* parent) : QObject(parent) {}

QString DownloadTracker::KeyFor(Kind kind, const QString& source, const QString& ref) {
  switch (kind) {
    case Kind::Game: return "game:" + ref;
    case Kind::Title: return source + ":" + ref;
    case Kind::Humble: return "humble:" + ref;
    case Kind::Launcher: return "launcher:" + source;
    case Kind::Tool: return "tool:" + source;
    case Kind::Runner: return "runner:" + source + "/" + ref;
  }
  return QString();
}

bool DownloadTracker::HandleEvent(const std::string& type, const std::string& data) {
  State state;
  if (InstallEvent install; MiradClient::ParseInstallEvent(type, data, &install)) {
    if (!ToState(install.state, &state)) return true;
    Entry& entry = Upsert(Kind::Game, QString(), QString::fromStdString(install.id));
    entry.state = state;
    entry.error = QString::fromStdString(install.error);
    if (state == State::Running) entry.bytes = 0;
    emit Changed(entry.key);
    return true;
  }
  if (RunnerDownloadEvent runner; MiradClient::ParseRunnerDownload(type, data, &runner)) {
    if (!ToState(runner.state, &state)) return true;
    // Kron4ek's variants share a tag, so key by the release's name.
    const std::string& ref = runner.name.empty() ? runner.tag : runner.name;
    Entry& entry = Upsert(Kind::Runner, QString::fromStdString(runner.kind), QString::fromStdString(ref));
    if (!runner.label.empty()) NoteTitle("runner:" + entry.source, entry.ref, QString::fromStdString(runner.label));
    entry.state = state;
    entry.error = QString::fromStdString(runner.error);
    emit Changed(entry.key);
    return true;
  }
  StoreEvent store;
  if (!MiradClient::ParseStoreEvent(type, data, &store)) return false;
  if (store.state == "progress") {
    Entry& entry = Upsert(Kind::Title, QString::fromStdString(store.source), QString::fromStdString(store.ref));
    entry.state = State::Running;
    entry.progress = store.progress;
    emit Changed(entry.key);
    return true;
  }
  if (!ToState(store.state, &state)) return true;
  const QString source = QString::fromStdString(store.source);
  const QString ref = QString::fromStdString(store.ref);
  Kind kind = Kind::Title;
  if (store.kind == "download") {
    kind = Kind::Humble;
  } else if (store.kind == "setup") {
    kind = type.starts_with("launcher.") ? Kind::Launcher : Kind::Tool;
  }
  Entry& entry = Upsert(kind, source, ref);
  entry.state = state;
  entry.update = store.update;
  entry.error = QString::fromStdString(store.error);
  if (state == State::Running) entry.progress = -1;
  const QString key = entry.key;
  if (kind == Kind::Title && NameFor(entry) == ref) ResolveNames(source);
  emit Changed(key);
  return true;
}

DownloadTracker::Entry& DownloadTracker::Upsert(Kind kind, const QString& source, const QString& ref) {
  const QString key = KeyFor(kind, source, ref);
  auto found = std::find_if(entries_.begin(), entries_.end(), [&key](const Entry& e) { return e.key == key; });
  if (found == entries_.end()) {
    Entry entry;
    entry.key = key;
    entry.kind = kind;
    entry.source = source;
    entry.ref = ref;
    entries_.insert(entries_.begin(), std::move(entry));
    found = entries_.begin();
  } else if (found != entries_.begin()) {
    // Newest activity first.
    std::rotate(entries_.begin(), found, found + 1);
    found = entries_.begin();
  }
  found->changed = QDateTime::currentDateTime();

  // Only game installers can say how far along they are.
  if (kind == Kind::Game) {
    if (poll_ == nullptr) {
      poll_ = new QTimer(this);
      poll_->setInterval(2000);
      connect(poll_, &QTimer::timeout, this, &DownloadTracker::Poll);
    }
    poll_->start();
  }
  return *found;
}

const DownloadTracker::Entry* DownloadTracker::Find(const QString& key) const {
  for (const Entry& entry : entries_) {
    if (entry.key == key) return &entry;
  }
  return nullptr;
}

int DownloadTracker::RunningCount() const {
  return static_cast<int>(
      std::count_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.state == State::Running; }));
}

void DownloadTracker::ClearFinished() {
  std::erase_if(entries_, [](const Entry& e) { return e.state != State::Running; });
  emit Changed(QString());
}

QString DownloadTracker::NameFor(const Entry& entry) const {
  const auto game = [this](const QString& id) {
    return game_name ? game_name(id.toStdString()) : QString();
  };
  const auto source = [this](const QString& id) { return source_name ? source_name(id) : id; };
  switch (entry.kind) {
    case Kind::Game: {
      const QString name = game(entry.ref);
      return name.isEmpty() ? entry.ref : name;
    }
    case Kind::Title: {
      if (const QString title = titles_.value(entry.source + ":" + entry.ref); !title.isEmpty()) return title;
      const QString name = game(entry.source + "-" + entry.ref);
      return name.isEmpty() ? entry.ref : name;
    }
    case Kind::Humble: {
      const QString title = titles_.value("humble:" + entry.ref);
      return title.isEmpty() ? QString("Humble Bundle purchase") : title;
    }
    case Kind::Launcher: return source(entry.source);
    case Kind::Tool: return ToolName(entry.source);
    case Kind::Runner: {
      const QString label = titles_.value("runner:" + entry.source + ":" + entry.ref);
      return label.isEmpty() ? entry.ref : label;
    }
  }
  return entry.ref;
}

void DownloadTracker::NoteTitle(const QString& source, const QString& ref, const QString& title) {
  titles_.insert(source + ":" + ref, title);
}

QString DownloadTracker::GameIdFor(const Entry& entry) {
  if (entry.kind == Kind::Game) return entry.ref;
  // Steam hands the install to its own client; the game is scanned in later.
  if (entry.kind == Kind::Title && entry.source != "steam") return entry.source + "-" + entry.ref;
  return QString();
}

void DownloadTracker::Poll() {
  bool any = false;
  for (const Entry& entry : entries_) {
    if (entry.kind != Kind::Game || entry.state != State::Running) continue;
    any = true;
    const QString key = entry.key;
    MiradClient::GetInstallProgressAsync(this, entry.ref.toStdString(), [this, key](InstallProgressResult progress) {
      auto found = std::find_if(entries_.begin(), entries_.end(), [&key](const Entry& e) { return e.key == key; });
      if (!progress.ok || found == entries_.end() || found->state != State::Running) return;
      found->bytes = progress.bytes_written;
      // In case the event was missed.
      if (progress.state == "finished") found->state = State::Finished;
      if (progress.state == "failed") found->state = State::Failed;
      emit Changed(key);
    });
  }
  if (!any) poll_->stop();
}

void DownloadTracker::ResolveNames(const QString& source) {
  if (asked_sources_.contains(source)) return;
  asked_sources_.insert(source);
  MiradClient::GetStoreLibraryAsync(this, source.toStdString(), [this, source](StoreLibraryResult result) {
    if (!result.ok) return;
    for (const StoreTitle& title : result.titles) {
      NoteTitle(source, QString::fromStdString(title.ref), QString::fromStdString(title.title));
    }
    emit Changed(QString());
  });
}

}  // namespace mira_gui
