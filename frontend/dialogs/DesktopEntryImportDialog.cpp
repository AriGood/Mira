#include "DesktopEntryImportDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "../ui/Notify.h"

namespace mira_gui {

namespace {
constexpr int kIdRole = Qt::UserRole;
}

DesktopEntryImportDialog::DesktopEntryImportDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Import desktop entries");
  setMinimumSize(520, 420);

  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(8);

  auto* explanation = new QLabel(
      "Already-installed application-menu entries mirad hasn't seen yet. This is how a Flatpak "
      "app gets added — its own entry carries everything needed to relaunch it.",
      this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  list_ = new QListWidget(this);
  layout->addWidget(list_, /*stretch=*/1);
  connect(list_, &QListWidget::itemChanged, this, &DesktopEntryImportDialog::UpdateImportEnabled);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  import_ = buttons->addButton("Import selected", QDialogButtonBox::ActionRole);
  import_->setEnabled(false);
  connect(import_, &QPushButton::clicked, this, &DesktopEntryImportDialog::Import);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  Load();
}

void DesktopEntryImportDialog::Load() {
  list_->clear();
  auto* loading = new QListWidgetItem("Loading…", list_);
  loading->setFlags(Qt::NoItemFlags);

  MiradClient::GetDesktopEntryCandidatesAsync(this, [this](DesktopEntryCandidatesResult result) {
    list_->clear();
    if (!result.ok) {
      notify::Failed(this, "Could not list desktop entries.", QString::fromStdString(result.error));
      return;
    }
    if (result.candidates.empty()) {
      auto* empty = new QListWidgetItem("Nothing new found.", list_);
      empty->setFlags(Qt::NoItemFlags);
      return;
    }
    for (const DesktopEntryCandidate& candidate : result.candidates) {
      const QString name = candidate.name.empty() ? QString::fromStdString(candidate.id)
                                                   : QString::fromStdString(candidate.name);
      auto* item = new QListWidgetItem(name, list_);
      item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
      item->setCheckState(Qt::Unchecked);
      item->setData(kIdRole, QString::fromStdString(candidate.id));
    }
  });
}

void DesktopEntryImportDialog::UpdateImportEnabled() {
  for (int i = 0; i < list_->count(); ++i) {
    if (list_->item(i)->checkState() == Qt::Checked) {
      import_->setEnabled(true);
      return;
    }
  }
  import_->setEnabled(false);
}

void DesktopEntryImportDialog::Import() {
  std::vector<std::string> ids;
  for (int i = 0; i < list_->count(); ++i) {
    const QListWidgetItem* item = list_->item(i);
    if (item->checkState() == Qt::Checked) ids.push_back(item->data(kIdRole).toString().toStdString());
  }
  if (ids.empty()) return;  // Import is disabled for this case

  setEnabled(false);
  import_->setText("Importing…");
  MiradClient::ImportDesktopEntriesAsync(this, ids, [this](DesktopEntryImportResult result) {
    setEnabled(true);
    import_->setText("Import selected");
    if (!result.ok) {
      notify::Failed(this, "Could not import.", QString::fromStdString(result.error));
      return;
    }
    notify::Toast(this, notify::Level::Success,
                  QString("Desktop entries: %1 added, %2 updated.")
                      .arg(result.added)
                      .arg(result.updated));
    accept();
  });
}

}  // namespace mira_gui
