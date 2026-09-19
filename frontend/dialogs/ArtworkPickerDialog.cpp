#include "ArtworkPickerDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "../ui/Notify.h"

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
  resize(680, 520);

  auto* layout = new QHBoxLayout(this);

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
  status_->setText("Loading…");
  RefreshPreview();
  event_stream_.Start(this,
                      [this](std::string type, std::string data) { HandleEvent(type, data); });
  Load();
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
      status_->setText(QString("No alternate %1 cached for this game yet.")
                            .arg(slot_ == "hero" ? "hero art" : "covers"));
      return;
    }
    status_->clear();

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
  status_->setText(QString("Applying candidate %1 of %2…")
                        .arg(index + 1)
                        .arg(candidates_.size()));
  const ArtCandidate& candidate = candidates_[index];
  MiradClient::SelectArtworkAsync(this, id_, slot_, candidate.id,
                                  [this, index](ArtworkSelectResult result) {
                                    if (result.ok) return;  // wait for the event instead
                                    SetBusy(false);
                                    pending_ = -1;
                                    notify::Failed(this, "Could not switch artwork.",
                                                   QString::fromStdString(result.error));
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

void ArtworkPickerDialog::SetBusy(bool busy) {
  busy_ = busy;
  list_->setEnabled(!busy && !candidates_.empty());
  fetch_button_->setEnabled(!busy && !fetching_);
}

void ArtworkPickerDialog::FetchFromSteamGridDb() {
  fetching_ = true;
  SetBusy(true);
  status_->setText("Fetching from SteamGridDB…");
  MiradClient::RefreshMetadataAsync(this, id_, /*announce=*/false,
                                    [this](MetadataRefreshResult result) {
                                      if (result.ok) return;  // wait for the event instead
                                      fetching_ = false;
                                      SetBusy(false);
                                      notify::Failed(this, "Could not fetch from SteamGridDB.",
                                                     QString::fromStdString(result.error));
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
      notify::Failed(this, "Could not fetch from SteamGridDB.", QString::fromStdString(event.error));
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
    notify::Failed(this, "Could not switch artwork.", QString::fromStdString(event.error));
    return;
  }

  current_ = index;
  status_->clear();

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
