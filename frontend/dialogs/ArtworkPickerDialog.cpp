#include "ArtworkPickerDialog.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "../ui/Theme.h"

namespace mira_gui {
namespace {

// Cover is 2:3 like CoverArt's own ratio; hero is a wide banner — SteamGridDB
// heroes run close to 2.3:1.
QSize PreviewSizeFor(const std::string& slot) {
  return slot == "hero" ? QSize(460, 200) : QSize(300, 450);
}

QString RowLabel(int index, const ArtCandidate& candidate, bool is_current) {
  QString style = QString::fromStdString(candidate.style);
  if (!style.isEmpty()) style[0] = style[0].toUpper();
  return QString("%1. %2%3 — %4×%5")
      .arg(index + 1)
      .arg(style.isEmpty() ? QString("Cover") : style)
      .arg(is_current ? " (current)" : QString())
      .arg(candidate.width)
      .arg(candidate.height);
}

}  // namespace

ArtworkPickerDialog::ArtworkPickerDialog(std::string game_id, std::string slot, QWidget* parent)
    : QDialog(parent), id_(std::move(game_id)), slot_(std::move(slot)) {
  setWindowTitle(slot_ == "hero" ? "Choose hero art" : "Choose cover art");
  setModal(false);
  resize(680, 560);

  auto* outer = new QVBoxLayout(this);
  // When the art is for the wrong game: SteamGridDB matched the name to
  // something else. Picking another game refetches every candidate.
  match_row_ = new QWidget(this);
  auto* match_layout = new QHBoxLayout(match_row_);
  match_layout->setContentsMargins(0, 0, 0, 0);
  match_layout->addWidget(new QLabel("SteamGridDB game:", match_row_));
  match_ = new QComboBox(match_row_);
  match_->setMinimumWidth(240);
  connect(match_, &QComboBox::activated, this, &ArtworkPickerDialog::ChooseMatch);
  match_layout->addWidget(match_, /*stretch=*/1);
  match_search_ = new QLineEdit(match_row_);
  match_search_->setPlaceholderText("Search another name…");
  match_search_->setClearButtonEnabled(true);
  connect(match_search_, &QLineEdit::returnPressed, this,
          [this] { LoadMatches(match_search_->text().trimmed()); });
  match_layout->addWidget(match_search_);
  match_row_->setVisible(false);
  outer->addWidget(match_row_);

  auto* layout = new QHBoxLayout();
  outer->addLayout(layout, /*stretch=*/1);

  list_ = new QListWidget(this);
  list_->setMinimumWidth(260);
  connect(list_, &QListWidget::currentRowChanged, this, [this](int row) { Select(row); });
  layout->addWidget(list_, /*stretch=*/1);

  auto* right = new QVBoxLayout();
  const QSize preview_size = PreviewSizeFor(slot_);
  preview_ = new QLabel(this);
  preview_->setAlignment(Qt::AlignCenter);
  preview_->setFixedSize(preview_size);
  right->addWidget(preview_, 0, Qt::AlignHCenter);

  status_ = new QLabel(this);
  status_->setAlignment(Qt::AlignCenter);
  status_->setProperty("role", "muted");
  status_->setWordWrap(true);
  right->addWidget(status_);

  fetch_button_ = new QPushButton("Fetch from SteamGridDB", this);
  fetch_button_->setVisible(false);
  connect(fetch_button_, &QPushButton::clicked, this, &ArtworkPickerDialog::FetchFromSteamGridDb);
  right->addWidget(fetch_button_);

  right->addStretch(1);

  auto* close = new QPushButton("Close", this);
  connect(close, &QPushButton::clicked, this, &QDialog::hide);
  right->addWidget(close);

  layout->addLayout(right, /*stretch=*/1);

  SetBusy(true);
  SetStatus("Loading…");
  RefreshPreview();
  event_stream_.Start(this,
                      [this](std::string type, std::string data) { HandleEvent(type, data); });
  Load();
  LoadMatches(QString());
}

void ArtworkPickerDialog::LoadMatches(const QString& query) {
  match_->setEnabled(false);
  MiradClient::GetGriddbMatchesAsync(this, id_, query.toStdString(), [this, query](GriddbMatchesResult result) {
    match_->setEnabled(true);
    // No key, or SteamGridDB unreachable: nothing to choose between.
    if (!result.ok) {
      if (!query.isEmpty()) SetStatus("Could not search SteamGridDB: " + QString::fromStdString(result.error), true);
      return;
    }
    match_row_->setVisible(true);
    match_->clear();
    for (const GriddbMatch& match : result.matches) {
      QString label = QString::fromStdString(match.name);
      if (match.year > 0) label += QString(" (%1)").arg(match.year);
      match_->addItem(label, QVariant::fromValue<qlonglong>(match.id));
      if (match.id == result.chosen) match_->setCurrentIndex(match_->count() - 1);
    }
    if (result.matches.empty()) match_->addItem("No matches for \"" + QString::fromStdString(result.query) + "\"");
    match_->setEnabled(!result.matches.empty());
    // A search's results aren't in use until one is picked.
    if (!query.isEmpty() && !result.matches.empty()) {
      match_->setCurrentIndex(-1);
      match_->setPlaceholderText("Pick the right game…");
      match_->showPopup();
    }
  });
}

void ArtworkPickerDialog::ChooseMatch(int index) {
  const QVariant id = match_->itemData(index);
  if (!id.isValid() || busy_) return;
  fetching_ = true;
  SetBusy(true);
  SetStatus("Fetching art for " + match_->itemText(index) + "…");
  MiradClient::SetGriddbMatchAsync(this, id_, id.toLongLong(), [this](GameActionResult result) {
    if (result.ok) return;  // game.metadata_ready reloads the list
    fetching_ = false;
    SetBusy(false);
    SetStatus("Could not switch games: " + QString::fromStdString(result.error), /*error=*/true);
  });
}

void ArtworkPickerDialog::Load() {
  // Not just on first call any more — FetchFromSteamGridDb() re-runs this
  // once the fetch it triggered lands, so a stale list_ can't be assumed.
  list_->clear();
  candidates_.clear();
  current_ = -1;
  MiradClient::GetMetadataAsync(this, id_, [this](GameMetadataResult result) {
    // candidates_ first: SetBusy reads it to decide whether the list should
    // end up enabled, so setting it after leaves the list stuck disabled.
    std::optional<std::int64_t> active_id;
    if (result.ok) {
      candidates_ = slot_ == "hero" ? result.metadata.hero_candidates : result.metadata.cover_candidates;
      active_id = slot_ == "hero" ? result.metadata.hero_active_candidate_id
                                  : result.metadata.cover_active_candidate_id;
    }
    SetBusy(false);
    fetch_button_->setVisible(candidates_.empty());
    if (candidates_.empty()) {
      SetStatus(QString("No alternate %1 cached for this game yet.")
                            .arg(slot_ == "hero" ? "hero art" : "covers"));
      return;
    }
    SetStatus(QString());

    list_->blockSignals(true);
    for (int i = 0; i < static_cast<int>(candidates_.size()); ++i) {
      const bool is_current = active_id && candidates_[i].id == *active_id;
      if (is_current) current_ = i;
      list_->addItem(RowLabel(i, candidates_[i], is_current));
    }
    if (current_ >= 0) list_->setCurrentRow(current_);
    list_->blockSignals(false);
  });
}

void ArtworkPickerDialog::Select(int index) {
  if (busy_ || index < 0 || index >= static_cast<int>(candidates_.size()) || index == current_) {
    return;
  }
  SetBusy(true);
  pending_ = index;
  SetStatus(QString("Applying candidate %1 of %2…")
                        .arg(index + 1)
                        .arg(candidates_.size()));
  const ArtCandidate& candidate = candidates_[index];
  MiradClient::SelectArtworkAsync(this, id_, slot_, candidate.id,
                                  [this, index](ArtworkSelectResult result) {
                                    if (result.ok) return;  // wait for the event instead
                                    SetBusy(false);
                                    pending_ = -1;
                                    SetStatus("Could not switch artwork: " + QString::fromStdString(result.error), /*error=*/true);
                                  });
}

void ArtworkPickerDialog::RefreshPreview() {
  MiradClient::GetArtworkSlotAsync(this, id_, slot_, [this](ArtworkResult result) {
    if (!result.ok) return;
    QPixmap pixmap;
    pixmap.loadFromData(reinterpret_cast<const uchar*>(result.bytes.data()),
                        static_cast<uint>(result.bytes.size()));
    preview_->setPixmap(
        pixmap.scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
  });
}

void ArtworkPickerDialog::SetStatus(const QString& text, bool error) {
  status_->setText(text);
  theme::SetStyleProperty(status_, "role", error ? "error" : "muted");
}

void ArtworkPickerDialog::SetBusy(bool busy) {
  busy_ = busy;
  list_->setEnabled(!busy && !candidates_.empty());
  fetch_button_->setEnabled(!busy && !fetching_);
}

void ArtworkPickerDialog::FetchFromSteamGridDb() {
  fetching_ = true;
  SetBusy(true);
  SetStatus("Fetching from SteamGridDB…");
  MiradClient::RefreshMetadataAsync(this, id_, /*announce=*/false,
                                    [this](MetadataRefreshResult result) {
                                      if (result.ok) return;  // wait for the event instead
                                      fetching_ = false;
                                      SetBusy(false);
                                      SetStatus("Could not fetch from SteamGridDB: " + QString::fromStdString(result.error), /*error=*/true);
                                    });
}

void ArtworkPickerDialog::HandleEvent(const std::string& type, const std::string& data) {
  if (type == "game.metadata_ready" || type == "game.metadata_failed") {
    if (!fetching_) return;
    MetadataEvent event;
    if (!MiradClient::ParseMetadataEvent(data, &event) || event.id != id_) return;
    fetching_ = false;
    if (type == "game.metadata_failed") {
      SetBusy(false);
      SetStatus("Could not fetch from SteamGridDB: " + QString::fromStdString(event.error), /*error=*/true);
      return;
    }
    Load();  // repopulates candidates_ and clears busy_ itself
    return;
  }

  if (type != "game.artwork_selected" && type != "game.artwork_select_failed") return;
  ArtworkSelectEvent event;
  if (!MiradClient::ParseArtworkSelectEvent(data, &event)) return;
  if (event.id != id_ || event.slot != slot_ || pending_ < 0) return;

  const int index = pending_;
  pending_ = -1;
  SetBusy(false);

  if (type == "game.artwork_select_failed") {
    SetStatus("Could not switch artwork: " + QString::fromStdString(event.error), /*error=*/true);
    return;
  }

  current_ = index;
  SetStatus(QString());

  // Relabel so "(current)" follows the pick, without re-triggering Select()
  // via currentRowChanged.
  list_->blockSignals(true);
  for (int i = 0; i < list_->count(); ++i) {
    list_->item(i)->setText(RowLabel(i, candidates_[i], i == current_));
  }
  list_->setCurrentRow(current_);
  list_->blockSignals(false);

  RefreshPreview();
}

}  // namespace mira_gui
