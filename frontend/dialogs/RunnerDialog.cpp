#include "RunnerDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "../ui/GamePresentation.h"
#include "../ui/Notify.h"
#include "../ui/Theme.h"

namespace {

constexpr int kTagRole = Qt::UserRole + 1;

QString FormatSize(std::int64_t bytes) {
  if (bytes <= 0) return "—";
  const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
  return mib >= 1024.0 ? QString("%1 GiB").arg(mib / 1024.0, 0, 'f', 1)
                       : QString("%1 MiB").arg(mib, 0, 'f', 0);
}

// The catalog's `published_at` is an ISO-8601 instant; only the date is
// worth a column.
QString FormatPublished(const std::string& published_at) {
  const QString text = QString::fromStdString(published_at);
  return text.left(10);
}

}  // namespace

RunnerDialog::RunnerDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Runners");
  resize(720, 520);

  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(8);

  auto* header = new QHBoxLayout();
  auto* kind_label = new QLabel("Runner kind", this);
  header->addWidget(kind_label);
  kind_ = new QComboBox(this);
  // Only the two kinds with a download source behind them. `native` has no
  // builds, and `steam` resolves its build per-game from Steam's own
  // prefix — neither is installable here.
  kind_->addItem("Proton", "proton");
  kind_->addItem("Wine", "wine");
  connect(kind_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
    RefreshInstalled();
    RefreshCatalog();
  });
  header->addWidget(kind_);
  header->addStretch(1);
  refresh_ = new QPushButton("Refresh", this);
  connect(refresh_, &QPushButton::clicked, this, [this] {
    RefreshInstalled();
    RefreshCatalog();
  });
  header->addWidget(refresh_);
  layout->addLayout(header);

  auto* installed_header = new QHBoxLayout();
  auto* installed_label = new QLabel("Installed", this);
  installed_label->setProperty("role", "section");
  installed_header->addWidget(installed_label);
  installed_header->addStretch(1);
  schema_ = new QPushButton("Accepted config keys…", this);
  connect(schema_, &QPushButton::clicked, this, &RunnerDialog::ShowSchema);
  installed_header->addWidget(schema_);
  layout->addLayout(installed_header);

  installed_ = new QTreeWidget(this);
  installed_->setColumnCount(3);
  installed_->setHeaderLabels({"Name", "Reference", "Version"});
  installed_->setRootIsDecorated(false);
  installed_->setAlternatingRowColors(true);
  installed_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  connect(installed_, &QTreeWidget::itemSelectionChanged, this, [this] {
    QTreeWidgetItem* item = installed_->currentItem();
    remove_->setEnabled(item != nullptr && (item->flags() & Qt::ItemIsSelectable));
  });
  layout->addWidget(installed_, /*stretch=*/1);

  auto* catalog_label = new QLabel("Available to install", this);
  catalog_label->setProperty("role", "section");
  layout->addWidget(catalog_label);

  catalog_ = new QTreeWidget(this);
  catalog_->setColumnCount(4);
  catalog_->setHeaderLabels({"Tag", "Published", "Size", "Checksum"});
  catalog_->setRootIsDecorated(false);
  catalog_->setAlternatingRowColors(true);
  catalog_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  connect(catalog_, &QTreeWidget::itemSelectionChanged, this,
          [this] { download_->setEnabled(catalog_->currentItem() != nullptr); });
  connect(catalog_, &QTreeWidget::itemDoubleClicked, this, [this] { DownloadSelected(); });
  layout->addWidget(catalog_, /*stretch=*/2);

  status_ = new QLabel(this);
  status_->setWordWrap(true);
  status_->setProperty("role", "muted");
  layout->addWidget(status_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  remove_ = buttons->addButton("Remove", QDialogButtonBox::ActionRole);
  remove_->setEnabled(false);
  connect(remove_, &QPushButton::clicked, this, &RunnerDialog::RemoveSelected);
  download_ = buttons->addButton("Download", QDialogButtonBox::ActionRole);
  download_->setEnabled(false);
  connect(download_, &QPushButton::clicked, this, &RunnerDialog::DownloadSelected);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  RefreshInstalled();
  RefreshCatalog();

  event_stream_.Start(this,
                      [this](std::string type, std::string data) { HandleEvent(type, data); });
}

std::string RunnerDialog::CurrentKind() const { return kind_->currentData().toString().toStdString(); }

void RunnerDialog::SetStatus(const QString& text, bool error) {
  status_->setText(text);
  mira_gui::theme::SetStyleProperty(status_, "role", error ? "error" : "muted");
}

void RunnerDialog::RefreshInstalled() {
  const std::string kind = CurrentKind();
  mira_gui::MiradClient::ListRunnersAsync(this, [this, kind](mira_gui::RunnersResult result) {
    installed_->clear();
    remove_->setEnabled(false);
    if (!result.ok) {
      SetStatus(QString::fromStdString(result.error), /*error=*/true);
      return;
    }
    for (const mira_gui::RunnerInfo& runner : result.runners) {
      if (runner.kind != kind) continue;
      auto* item = new QTreeWidgetItem(installed_);
      item->setText(0, QString::fromStdString(runner.name));
      item->setText(1, QString::fromStdString(runner.reference));
      item->setText(2, QString::fromStdString(runner.version));
    }
    if (installed_->topLevelItemCount() == 0) {
      auto* item = new QTreeWidgetItem(installed_);
      item->setText(0, "None installed");
      item->setForeground(0, mira_gui::theme::Current().text_muted);
      item->setFlags(Qt::NoItemFlags);
    }
  });
}

void RunnerDialog::RefreshCatalog() {
  catalog_->clear();
  download_->setEnabled(false);
  // This is the one call that leaves the machine, so say so rather than
  // looking hung for a few seconds.
  SetStatus("Fetching available builds from the release source…");

  const std::string kind = CurrentKind();
  mira_gui::MiradClient::GetRunnerCatalogAsync(
      this, kind, [this](mira_gui::RunnerCatalogResult result) {
        if (!result.ok) {
          SetStatus(QString("Could not list available builds: %1")
                        .arg(QString::fromStdString(result.error)),
                    /*error=*/true);
          return;
        }
        for (const mira_gui::RunnerRelease& release : result.releases) {
          auto* item = new QTreeWidgetItem(catalog_);
          item->setText(0, QString::fromStdString(release.tag));
          item->setData(0, kTagRole, QString::fromStdString(release.tag));
          item->setText(1, FormatPublished(release.published_at));
          item->setText(2, FormatSize(release.size_bytes));
          // A release without one still installs; it just can't be verified
          // before it is, which is worth seeing before downloading 500 MB.
          item->setText(3, release.has_checksum ? "yes" : "no");
          if (!release.has_checksum) item->setForeground(3, mira_gui::theme::Current().warning);
        }
        SetStatus(QString("%1 build(s) available.").arg(result.releases.size()));
      });
}

void RunnerDialog::DownloadSelected() {
  QTreeWidgetItem* item = catalog_->currentItem();
  if (item == nullptr) return;

  const std::string tag = item->data(0, kTagRole).toString().toStdString();
  const std::string kind = CurrentKind();
  if (tag.empty() || downloading_.contains(tag)) return;

  downloading_.insert(tag);
  download_->setEnabled(false);
  SetStatus(QString("Starting download of %1…").arg(QString::fromStdString(tag)));

  mira_gui::MiradClient::DownloadRunnerAsync(
      this, kind, tag, [this, tag](mira_gui::RunnerDownloadResult result) {
        if (result.ok) return;  // progress arrives on the event stream
        downloading_.erase(tag);
        download_->setEnabled(catalog_->currentItem() != nullptr);
        SetStatus(QString("Download failed to start: %1")
                      .arg(QString::fromStdString(result.error)),
                  /*error=*/true);
      });
}

void RunnerDialog::RemoveSelected() {
  QTreeWidgetItem* item = installed_->currentItem();
  if (item == nullptr) return;

  const std::string kind = CurrentKind();
  const std::string name = item->text(0).toStdString();
  if (!mira_gui::notify::Confirm(
          this, "Remove runner",
          QString("Remove \"%1\"? This deletes its installed files.").arg(item->text(0)), "Remove",
          /*destructive=*/true)) {
    return;
  }

  mira_gui::MiradClient::DeleteRunnerAsync(
      this, kind, name, [this](mira_gui::RunnerRemoveResult result) {
        if (!result.ok) {
          SetStatus(QString("Could not remove that runner: %1")
                        .arg(QString::fromStdString(result.error)),
                    /*error=*/true);
          return;
        }
        RefreshInstalled();
      });
}

void RunnerDialog::ShowSchema() {
  const std::string kind = CurrentKind();
  mira_gui::MiradClient::GetRunnerSchemaAsync(
      this, kind, [this, kind](mira_gui::RunnerSchemaResult result) {
        if (!result.ok) {
          SetStatus("Could not read this runner kind's config keys: " +
                        QString::fromStdString(result.error),
                    /*error=*/true);
          return;
        }
        if (result.entries.empty()) {
          mira_gui::notify::Info(this, "Accepted config keys",
                                 "This runner kind takes no extra config keys.");
          return;
        }
        QString text;
        for (const mira_gui::RunnerSchemaEntry& entry : result.entries) {
          if (!text.isEmpty()) text += '\n';
          text += QString("%1 (%2) — %3")
                      .arg(QString::fromStdString(entry.key), QString::fromStdString(entry.type),
                           QString::fromStdString(entry.doc));
        }
        mira_gui::notify::Info(
            this, QString("Accepted config keys (%1)").arg(QString::fromStdString(kind)), text);
      });
}

void RunnerDialog::HandleEvent(const std::string& type, const std::string& data) {
  mira_gui::RunnerDownloadEvent event;
  if (!mira_gui::MiradClient::ParseRunnerDownload(type, data, &event)) return;

  const QString tag = QString::fromStdString(event.tag);
  if (event.state == "started") {
    SetStatus(QString("Downloading %1…").arg(tag));
    return;
  }

  downloading_.erase(event.tag);
  download_->setEnabled(catalog_->currentItem() != nullptr);
  if (event.state == "failed") {
    SetStatus(QString("Downloading %1 failed: %2").arg(tag, QString::fromStdString(event.error)),
              /*error=*/true);
    return;
  }
  SetStatus(QString("Installed %1.").arg(tag));
  // Discoverable once on disk, and the list call re-discovers every time.
  RefreshInstalled();
}
