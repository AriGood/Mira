#include "ManageSourcesDialog.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "../ui/Notify.h"
#include "../ui/Theme.h"

namespace {

using Kind = mira_gui::SourceInfo::Kind;

QString KindHeading(Kind kind) {
  switch (kind) {
    case Kind::Store:
      return "STORES";
    case Kind::Launcher:
      return "STORE LAUNCHERS";
    case Kind::Local:
      return "ON THIS PC";
  }
  return {};
}

QString Ago(std::int64_t unix_seconds) {
  const qint64 seconds = QDateTime::currentSecsSinceEpoch() - unix_seconds;
  if (seconds < 60) return "just now";
  if (seconds < 3600) return QString("%1 min ago").arg(seconds / 60);
  if (seconds < 86400) return QString("%1 h ago").arg(seconds / 3600);
  return QString("%1 days ago").arg(seconds / 86400);
}

}  // namespace

ManageSourcesDialog::ManageSourcesDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Manage sources");
  setMinimumSize(640, 520);
  auto* layout = new QVBoxLayout(this);

  auto* intro = new QLabel(
      "Connect stores and launchers, choose what shows in the sidebar and in which order, or "
      "remove a source.",
      this);
  intro->setProperty("role", "muted");
  intro->setWordWrap(true);
  layout->addWidget(intro);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* content = new QWidget();
  rows_ = new QVBoxLayout(content);
  rows_->setSpacing(4);
  scroll->setWidget(content);
  layout->addWidget(scroll, /*stretch=*/1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

void ManageSourcesDialog::SetEntries(const std::vector<Entry>& entries) {
  while (QLayoutItem* item = rows_->takeAt(0)) {
    if (item->widget() != nullptr) item->widget()->deleteLater();
    delete item;
  }
  QWidget* content = rows_->parentWidget();
  for (const Kind kind : {Kind::Store, Kind::Launcher, Kind::Local}) {
    std::vector<const Entry*> group;
    for (const Entry& entry : entries) {
      if (entry.source.kind == kind) group.push_back(&entry);
    }
    auto* heading = new QLabel(KindHeading(kind), content);
    heading->setProperty("role", "muted");
    heading->setStyleSheet("font-weight: 600; letter-spacing: 0.04em; margin-top: 10px;");
    rows_->addWidget(heading);
    for (size_t i = 0; i < group.size(); ++i) {
      rows_->addWidget(BuildRow(*group[i], i == 0, i + 1 == group.size()));
    }
  }
  rows_->addStretch(1);
}

QWidget* ManageSourcesDialog::BuildRow(const Entry& entry, bool first, bool last) {
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();
  QWidget* content = rows_->parentWidget();
  auto* row = new QWidget(content);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(8, 6, 8, 6);
  layout->setSpacing(10);

  auto* badge = new QLabel(entry.source.name.left(1), row);
  badge->setFixedSize(30, 30);
  badge->setAlignment(Qt::AlignCenter);
  QColor fill = entry.source.color;
  if (!entry.ready || !entry.enabled) fill.setAlphaF(0.55);
  badge->setStyleSheet(
      QString("background: %1; color: white; border-radius: 7px; font-weight: 700;")
          .arg(fill.name(QColor::HexArgb)));
  layout->addWidget(badge);

  auto* text = new QVBoxLayout();
  text->setSpacing(1);
  auto* name = new QLabel(entry.source.name, row);
  name->setStyleSheet("font-weight: 600;");
  if (!entry.ready || !entry.enabled) name->setProperty("role", "muted");
  text->addWidget(name);
  QStringList details;
  if (!entry.enabled) {
    details << "Off";
  } else if (entry.ready) {
    details << QString("%1 game%2").arg(entry.games).arg(entry.games == 1 ? "" : "s");
    if (!entry.account.empty()) details << "signed in as " + QString::fromStdString(entry.account);
    if (entry.imported_at > 0) details << "imported " + Ago(entry.imported_at);
  } else {
    details << "Not set up";
  }
  auto* status = new QLabel(details.join("  ·  "), row);
  status->setObjectName("status");
  if (entry.enabled && entry.ready) {
    status->setStyleSheet(QString("color: %1;").arg(tokens.success.name()));
  } else {
    status->setProperty("role", "muted");
  }
  text->addWidget(status);
  layout->addLayout(text, /*stretch=*/1);

  const QString id = entry.source.id;
  auto* enabled = new QCheckBox("On", row);
  enabled->setChecked(entry.enabled);
  enabled->setToolTip("Turn the source off entirely: no sidebar row and no imports.");
  connect(enabled, &QCheckBox::toggled, this, [this, id](bool on) { emit EnabledToggled(id, on); });
  layout->addWidget(enabled);

  if (entry.ready && entry.enabled) {
    auto* in_sidebar = new QCheckBox("In sidebar", row);
    in_sidebar->setChecked(entry.in_sidebar);
    connect(in_sidebar, &QCheckBox::toggled, this,
            [this, id](bool on) { emit SidebarToggled(id, on); });
    layout->addWidget(in_sidebar);
  }

  auto* open = new QPushButton(entry.ready ? "Open" : "Set up", row);
  open->setEnabled(entry.enabled);
  connect(open, &QPushButton::clicked, this, [this, id] {
    accept();
    emit OpenRequested(id);
  });
  layout->addWidget(open);

  auto* more = new QToolButton(row);
  more->setText("⋯");
  more->setToolTip("More");
  more->setPopupMode(QToolButton::InstantPopup);
  auto* menu = new QMenu(more);
  menu->addAction("Move up", this, [this, id] { emit MoveRequested(id, -1); })->setEnabled(!first);
  menu->addAction("Move down", this, [this, id] { emit MoveRequested(id, +1); })->setEnabled(!last);
  if (entry.ready && entry.enabled && entry.source.id != "humble") {
    menu->addSeparator();
    menu->addAction("Import games", this, [this, entry, status] { Import(entry, status); });
  }
  if (entry.ready) {
    menu->addSeparator();
    menu->addAction("Remove…", this, [this, entry] { Remove(entry); });
  }
  more->setMenu(menu);
  layout->addWidget(more);
  return row;
}

void ManageSourcesDialog::Import(const Entry& entry, QWidget* status_holder) {
  auto* status = qobject_cast<QLabel*>(status_holder);
  if (status != nullptr) status->setText("Importing…");
  const QString id = entry.source.id;
  const auto done = [this, id, status](bool ok, const std::string& error, int added, int updated) {
    if (status != nullptr) {
      status->setText(ok ? QString("Imported: %1 added, %2 updated").arg(added).arg(updated)
                         : "Could not import: " + QString::fromStdString(error));
    }
    if (ok) emit Imported(id);
  };
  const std::string source = id.toStdString();
  using namespace mira_gui;
  if (source == "steam") {
    MiradClient::ScanSteamAsync(
        this, [done](SteamScanResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (source == "lutris") {
    MiradClient::ImportLutrisAsync(
        this, [done](LutrisImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (entry.source.kind == Kind::Launcher) {
    MiradClient::ImportLauncherAsync(
        this, source, [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else {
    MiradClient::ImportStoreAsync(
        this, source, [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  }
}

void ManageSourcesDialog::Remove(const Entry& entry) {
  const QString id = entry.source.id;
  const QString name = entry.source.name;
  mira_gui::MiradClient::GetRemovalPlanAsync(
      this, id.toStdString(), [this, id, name](mira_gui::RemovalPlanResult plan) {
        if (!plan.ok) {
          mira_gui::notify::Failed(this, "Could not plan the removal.",
                                   QString::fromStdString(plan.error));
          return;
        }
        QStringList lines;
        const auto uninstalled =
            std::ranges::count_if(plan.games, [](const auto& g) { return !g.deletes.empty(); });
        if (uninstalled > 0)
          lines
              << QString("Uninstalls %1 game%2:").arg(uninstalled).arg(uninstalled == 1 ? "" : "s");
        for (const auto& game : plan.games) {
          if (!game.deletes.empty()) lines << "  • " + QString::fromStdString(game.name);
        }
        const auto dropped = static_cast<qsizetype>(plan.games.size()) - uninstalled;
        if (dropped > 0) {
          lines << QString("Removes %1 game%2 from Mira only (their files stay where they are).")
                       .arg(dropped)
                       .arg(dropped == 1 ? "" : "s");
        }
        if (!plan.launcher_dir.empty()) lines << "Deletes " + name + " itself.";
        if (plan.signs_out) lines << "Signs you out of " + name + ".";
        if (!plan.kept.empty()) lines << "Keeps game data and saves (prefixes stay on disk).";
        lines << name + " is turned off; turn it on again here any time.";
        if (!mira_gui::notify::Confirm(this, "Remove " + name, lines.join("\n"), "Remove",
                                       /*destructive=*/true)) {
          return;
        }
        mira_gui::MiradClient::RemoveSourceAsync(
            this, id.toStdString(), [this, id, name](mira_gui::RemoveSourceResult r) {
              if (!r.ok) {
                mira_gui::notify::Failed(this, "Could not remove " + name + ".",
                                         QString::fromStdString(r.error));
                return;
              }
              if (!r.problems.empty()) {
                QStringList problems;
                for (const std::string& problem : r.problems)
                  problems << QString::fromStdString(problem);
                mira_gui::notify::Failed(this, name + " was removed, but some steps failed.",
                                         problems.join("\n"));
              }
              emit Removed(id);
            });
      });
}
