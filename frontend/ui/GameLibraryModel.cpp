#include "GameLibraryModel.h"

#include <QTimer>

#include <algorithm>

#include "GamePresentation.h"
#include "GameTileDelegate.h"
#include "LibrarySort.h"

namespace mira_gui {
namespace {

bool HasTag(const GameSummary& game, std::string_view tag) { return std::ranges::contains(game.tags, tag); }

// A pinned game's tag. "favorite" because Lutris imports its favorites under it.
constexpr std::string_view kPinnedTag = "favorite";

}  // namespace

GameLibraryModel::GameLibraryModel(QObject* parent) : QAbstractTableModel(parent) {}

void GameLibraryModel::Replace(const std::vector<GameSummary>& games) {
  std::unordered_map<std::string, bool> listed;
  for (const GameSummary& game : games) listed[game.id] = true;
  std::vector<std::string> gone;
  for (const GameSummary& game : games_) {
    if (!listed.contains(game.id)) gone.push_back(game.id);
  }
  Remove(gone);
  Upsert(games);
}

void GameLibraryModel::Upsert(const std::vector<GameSummary>& games) {
  std::vector<const GameSummary*> added;
  std::unordered_map<std::string, std::size_t> added_at;  // an id repeated in one batch keeps its last copy
  for (const GameSummary& game : games) {
    const auto found = rows_.find(game.id);
    if (found == rows_.end()) {
      const auto [slot, inserted] = added_at.try_emplace(game.id, added.size());
      if (inserted) {
        added.push_back(&game);
      } else {
        added[slot->second] = &game;
      }
      continue;
    }
    if (games_[found->second] == game) continue;
    games_[found->second] = game;
    emit dataChanged(index(found->second, 0), index(found->second, kColumnCount - 1));
  }
  if (!added.empty()) {
    const int first = static_cast<int>(games_.size());
    beginInsertRows(QModelIndex(), first, first + static_cast<int>(added.size()) - 1);
    for (const GameSummary* game : added) {
      rows_[game->id] = static_cast<int>(games_.size());
      games_.push_back(*game);
    }
    endInsertRows();
  }
  NoteChanged();
}

void GameLibraryModel::Remove(const std::vector<std::string>& ids) {
  std::vector<int> rows;
  for (const std::string& id : ids) {
    if (const auto found = rows_.find(id); found != rows_.end()) rows.push_back(found->second);
  }
  if (rows.empty()) return;
  std::ranges::sort(rows, std::greater{});
  if (rows.size() == games_.size()) {
    beginResetModel();
    games_.clear();
    RebuildIndex();
    endResetModel();
    NoteChanged();
    return;
  }
  // Highest first, one begin/end per contiguous run, with rows_ already true when the signal fires.
  for (std::size_t i = 0; i < rows.size();) {
    const int last = rows[i];
    int first = last;
    for (++i; i < rows.size() && rows[i] == first - 1; ++i) --first;
    beginRemoveRows(QModelIndex(), first, last);
    games_.erase(games_.begin() + first, games_.begin() + last + 1);
    RebuildIndex();
    endRemoveRows();
  }
  NoteChanged();
}

void GameLibraryModel::RemoveSource(const std::string& source) {
  std::vector<std::string> ids;
  for (const GameSummary& game : games_) {
    if (game.source == source) ids.push_back(game.id);
  }
  Remove(ids);
}

void GameLibraryModel::SetRunning(const std::string& id, bool running) {
  const auto found = rows_.find(id);
  if (found == rows_.end() || games_[found->second].running == running) return;
  games_[found->second].running = running;
  emit dataChanged(index(found->second, 0), index(found->second, kColumnCount - 1));
  NoteChanged();
}

void GameLibraryModel::Touch(const std::string& id) {
  if (const auto found = rows_.find(id); found != rows_.end()) {
    emit dataChanged(index(found->second, 0), index(found->second, 0));
  }
}

const GameSummary* GameLibraryModel::Find(const std::string& id) const {
  const auto found = rows_.find(id);
  return found != rows_.end() ? &games_[found->second] : nullptr;
}

QModelIndex GameLibraryModel::IndexOf(const std::string& id) const {
  const auto found = rows_.find(id);
  return found != rows_.end() ? index(found->second, 0) : QModelIndex();
}

int GameLibraryModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(games_.size());
}

int GameLibraryModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : kColumnCount; }

QVariant GameLibraryModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= static_cast<int>(games_.size())) return {};
  const GameSummary& game = games_[index.row()];
  switch (role) {
    case GameTileDelegate::IdRole: return QString::fromStdString(game.id);
    case GameTileDelegate::NameRole: return QString::fromStdString(game.name);
    case GameTileDelegate::StatusRole: return QString::fromStdString(game.status);
    case GameTileDelegate::RunningRole: return game.running;
    case GameTileDelegate::PinnedRole: return HasTag(game, kPinnedTag);
    case GameTileDelegate::StatusTextRole: return status_text ? status_text(game.id) : QString();
    case GameTileDelegate::SourceRole: return QString::fromStdString(game.source);
    default: break;
  }
  const int column = index.column();
  if (role == Qt::DisplayRole) {
    switch (column) {
      case kName: return QString::fromStdString(game.name);
      case kStatus: return game.running ? QString("Playing") : StatusLabel(game.status);
      case kPlatform: return StatusLabel(game.platform);
      case kRunner: return game.runner_ref.empty() ? QString("Auto") : QString::fromStdString(game.runner_ref);
      case kLastPlayed: return FormatLastPlayed(game.last_played_at);
      case kPlaytime: return FormatPlaytime(game.play_seconds);
      default: return {};
    }
  }
  if (role == Qt::ForegroundRole && column == kStatus) return StatusColor(game.running ? "running" : game.status);
  if (role == Qt::ToolTipRole && column == kStatus && !game.last_error.empty()) {
    return QString::fromStdString(game.last_error);
  }
  return {};
}

QVariant GameLibraryModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* const kHeaders[] = {"Name", "Status", "Platform", "Runner", "Last Played", "Playtime"};
  return section >= 0 && section < kColumnCount ? QString(kHeaders[section]) : QVariant();
}

void GameLibraryModel::RebuildIndex() {
  rows_.clear();
  for (int row = 0; row < static_cast<int>(games_.size()); ++row) rows_[games_[row].id] = row;
}

void GameLibraryModel::NoteChanged() {
  if (change_pending_) return;
  change_pending_ = true;
  QTimer::singleShot(0, this, [this] {
    change_pending_ = false;
    emit Changed();
  });
}

GameFilterProxy::GameFilterProxy(GameLibraryModel* library, QObject* parent)
    : QSortFilterProxyModel(parent), library_(library) {
  setSourceModel(library);
  // Re-sorted and re-filtered as rows change, moving them rather than resetting.
  setDynamicSortFilter(true);
  QSortFilterProxyModel::sort(0, Qt::AscendingOrder);
}

bool GameFilterProxy::MatchesKey(const GameSummary& game, const QString& key) {
  // Store launchers (Battle.net, ...) live on their source pages, not here.
  if (game.source == "launcher") return false;
  if (key == "hidden") return HasTag(game, "hidden");
  // Every other filter excludes a hidden game: "not displayed by default"
  // means not in "All games" either, not just off the initial screen.
  if (HasTag(game, "hidden")) return false;
  if (key == "all") return true;
  if (key == "running") return game.running;
  if (key == "never") return !game.last_played_at.has_value();
  if (key == "attention") return game.status == "needs_install" || game.status == "broken" || game.status == "missing";
  return key == QLatin1StringView(game.status.data(), static_cast<qsizetype>(game.status.size()));
}

void GameFilterProxy::ChangeFilter(const std::function<void()>& change) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  beginFilterChange();
  change();
  endFilterChange(Direction::Rows);
#else
  change();
  invalidateFilter();
#endif
}

void GameFilterProxy::SetFilterKey(const QString& key) {
  if (key == key_) return;
  ChangeFilter([&] { key_ = key; });
}

void GameFilterProxy::SetSearch(const QString& text) {
  const QString trimmed = text.trimmed();
  if (trimmed == search_) return;
  ChangeFilter([&] { search_ = trimmed; });
}

void GameFilterProxy::SetSource(const std::string& source) {
  ChangeFilter([&] { source_ = source; });
}

void GameFilterProxy::SetSort(const std::string& key, bool descending) {
  if (key == sort_key_ && descending == descending_ && !column_sort_) return;
  sort_key_ = key;
  descending_ = descending;
  column_sort_ = false;
  QSortFilterProxyModel::sort(0, Qt::AscendingOrder);
  invalidate();
}

void GameFilterProxy::sort(int column, Qt::SortOrder order) {
  // No column (a header with no indicator) means the sidebar's order, not unsorted.
  column_sort_ = column >= 0;
  QSortFilterProxyModel::sort(std::max(column, 0), column_sort_ ? order : Qt::AscendingOrder);
}

const GameSummary* GameFilterProxy::GameAt(const QModelIndex& index) const {
  if (!index.isValid()) return nullptr;
  const QModelIndex source = mapToSource(index);
  return source.isValid() && source.row() < static_cast<int>(library_->Games().size())
             ? &library_->Games()[source.row()]
             : nullptr;
}

bool GameFilterProxy::filterAcceptsRow(int source_row, const QModelIndex&) const {
  const GameSummary& game = library_->Games()[source_row];
  if (!search_.isEmpty() && !QString::fromStdString(game.name).contains(search_, Qt::CaseInsensitive)) return false;
  if (!source_.empty()) return game.source == source_;
  return MatchesKey(game, key_);
}

bool GameFilterProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const {
  const auto& games = library_->Games();
  const GameSummary& a = games[left.row()];
  const GameSummary& b = games[right.row()];
  if (column_sort_) {
    if (left.column() == GameLibraryModel::kLastPlayed) {
      if (a.last_played_at != b.last_played_at) return a.last_played_at.value_or(-1) < b.last_played_at.value_or(-1);
    } else if (left.column() == GameLibraryModel::kPlaytime) {
      if (a.play_seconds != b.play_seconds) return a.play_seconds < b.play_seconds;
    } else if (const int order = QString::compare(left.data().toString(), right.data().toString(), Qt::CaseInsensitive);
               order != 0) {
      return order < 0;
    }
  }
  return GameLess(a, b, sort_key_, descending_);
}

}  // namespace mira_gui
