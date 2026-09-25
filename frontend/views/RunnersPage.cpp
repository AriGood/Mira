#include "RunnersPage.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

#include "../client/MiradClient.h"
#include "../ui/DownloadTracker.h"
#include "../ui/Notify.h"
#include "../ui/Theme.h"

namespace mira_gui {
namespace {

constexpr const char* kDefaultKey = "default_runner.windows";

QString FormatSize(std::int64_t bytes) {
  if (bytes <= 0) return QString();
  const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
  return mib >= 1024.0 ? QString("%1 GiB").arg(mib / 1024.0, 0, 'f', 1) : QString("%1 MiB").arg(mib, 0, 'f', 0);
}

QString FormatDate(const std::string& iso) {
  const QDateTime when = QDateTime::fromString(QString::fromStdString(iso), Qt::ISODate);
  return when.isValid() ? QLocale().toString(when.date(), "MMM d, yyyy") : QString();
}

QLabel* Muted(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", "muted");
  label->setWordWrap(true);
  return label;
}

void ClearLayout(QVBoxLayout* layout) {
  while (QLayoutItem* item = layout->takeAt(0)) {
    if (item->widget() != nullptr) item->widget()->deleteLater();
    delete item;
  }
}

// A titled card; `body` receives its content.
QFrame* Card(const QString& title, QWidget* trailing, QVBoxLayout** body, QWidget* parent) {
  auto* card = new QFrame(parent);
  card->setObjectName("runners_card");
  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(18, 16, 18, 16);
  layout->setSpacing(10);
  auto* header = new QHBoxLayout();
  auto* heading = new QLabel(title, card);
  heading->setProperty("role", "section");
  // Same height with or without a button beside it, so both cards line up.
  heading->setMinimumHeight(32);
  header->addWidget(heading, /*stretch=*/1);
  if (trailing != nullptr) header->addWidget(trailing);
  layout->addLayout(header);

  auto* scroll = new QScrollArea(card);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");
  auto* content = new QWidget();
  *body = new QVBoxLayout(content);
  (*body)->setContentsMargins(0, 0, 0, 0);
  (*body)->setSpacing(8);
  scroll->setWidget(content);
  layout->addWidget(scroll, /*stretch=*/1);
  return card;
}

}  // namespace

RunnersPage::RunnersPage(DownloadTracker* downloads, QWidget* parent) : QWidget(parent), downloads_(downloads) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(28, 22, 28, 22);
  layout->setSpacing(18);

  auto* header = new QHBoxLayout();
  auto* title = new QLabel("Runners", this);
  title->setObjectName("page_title");
  header->addWidget(title, /*stretch=*/1);
  // Only the two kinds with builds to download. `native` has none, and
  // `steam` resolves its build per game from Steam's own prefix.
  auto* segmented = new QWidget(this);
  segmented->setObjectName("segmented");
  auto* segmented_layout = new QHBoxLayout(segmented);
  segmented_layout->setContentsMargins(3, 3, 3, 3);
  segmented_layout->setSpacing(3);
  kinds_ = new QButtonGroup(this);
  for (const auto& [label, kind] : {std::pair{"Proton", "proton"}, std::pair{"Wine", "wine"}}) {
    auto* button = new QPushButton(label, segmented);
    button->setCheckable(true);
    button->setProperty("kind", kind);
    kinds_->addButton(button);
    segmented_layout->addWidget(button);
  }
  kinds_->buttons().first()->setChecked(true);
  connect(kinds_, &QButtonGroup::buttonClicked, this, [this] {
    catalog_loaded_ = false;
    releases_.clear();
    RebuildInstalled();
    RefreshSources();
    RebuildTools();
  });
  header->addWidget(segmented);
  layout->addLayout(header);

  auto* columns = new QHBoxLayout();
  columns->setSpacing(18);

  QVBoxLayout* installed_body = nullptr;
  QFrame* installed = Card("Installed", nullptr, &installed_body, this);
  installed_list_ = new QVBoxLayout();
  installed_list_->setSpacing(8);
  installed_body->addLayout(installed_list_);
  installed_body->addStretch(1);
  default_note_ = Muted(QString(), installed);
  default_note_->setTextFormat(Qt::RichText);
  connect(default_note_, &QLabel::linkActivated, this, [this] {
    MiradClient::ResetConfigKeyAsync(this, kDefaultKey, [this](PatchConfigResult result) {
      if (!result.ok) {
        SetStatus("Could not reset the default: " + QString::fromStdString(result.error), true);
        return;
      }
      RefreshInstalled();
    });
  });
  installed->layout()->addWidget(default_note_);
  columns->addWidget(installed, /*stretch=*/1);

  auto* catalog_actions = new QWidget(this);
  auto* catalog_actions_layout = new QHBoxLayout(catalog_actions);
  catalog_actions_layout->setContentsMargins(0, 0, 0, 0);
  catalog_actions_layout->setSpacing(8);
  source_ = new QComboBox(catalog_actions);
  source_->setToolTip("Where builds are downloaded from");
  connect(source_, &QComboBox::activated, this, [this] { RefreshCatalog(); });
  catalog_actions_layout->addWidget(source_);
  auto* refresh = new QPushButton("Refresh", catalog_actions);
  connect(refresh, &QPushButton::clicked, this, &RunnersPage::Refresh);
  catalog_actions_layout->addWidget(refresh);
  QVBoxLayout* catalog_body = nullptr;
  QFrame* catalog = Card("Get more", catalog_actions, &catalog_body, this);
  catalog_list_ = new QVBoxLayout();
  catalog_list_->setSpacing(0);
  catalog_body->addLayout(catalog_list_);
  catalog_body->addStretch(1);
  status_ = Muted(QString(), catalog);
  catalog->layout()->addWidget(status_);
  columns->addWidget(catalog, /*stretch=*/1);

  layout->addLayout(columns, /*stretch=*/1);

  // What runners need besides a build. Shown only while one is missing.
  tools_list_ = new QVBoxLayout();
  tools_list_->setSpacing(8);
  layout->addLayout(tools_list_);

  connect(downloads_, &DownloadTracker::Changed, this, &RunnersPage::DownloadChanged);
  Refresh();
}

void RunnersPage::SetGames(const std::vector<GameSummary>& games) {
  games_ = games;
  RebuildInstalled();
}

std::string RunnersPage::CurrentKind() const {
  return kinds_->checkedButton()->property("kind").toString().toStdString();
}

void RunnersPage::SetStatus(const QString& text, bool error) {
  status_->setText(text);
  theme::SetStyleProperty(status_, "role", error ? "error" : "muted");
}

void RunnersPage::Refresh() {
  RefreshInstalled();
  RefreshSources();
  RefreshUpdates();
  RefreshTools();
}

void RunnersPage::RefreshSources() {
  const std::string kind = CurrentKind();
  const auto fill = [this] {
    const std::string kind = CurrentKind();
    const QString chosen = source_->currentData().toString();
    source_->clear();
    for (const RunnerSourceInfo& info : sources_[kind]) {
      source_->addItem(QString::fromStdString(info.label), QString::fromStdString(info.id));
    }
    if (const int at = source_->findData(chosen); at >= 0) source_->setCurrentIndex(at);
    RefreshCatalog();
  };
  if (sources_.contains(kind)) return fill();
  MiradClient::ListRunnerSourcesAsync(this, kind, [this, kind, fill](RunnerSourcesResult result) {
    if (!result.ok) {
      SetStatus("Could not list sources: " + QString::fromStdString(result.error), true);
      return;
    }
    sources_[kind] = result.sources;
    if (kind == CurrentKind()) fill();
  });
}

void RunnersPage::RefreshUpdates() {
  MiradClient::GetRunnerUpdatesAsync(this, [this](RunnerUpdatesResult result) {
    if (!result.ok) return;  // offline; the catalog says so
    updates_ = result.updates;
    RebuildInstalled();
  });
}

void RunnersPage::RefreshTools() {
  MiradClient::ListRunnerToolsAsync(this, [this](RunnerToolsResult result) {
    if (!result.ok) return;
    tools_ = result.tools;
    RebuildTools();
  });
}

QString RunnersPage::SourceLabel(const std::string& id) const {
  for (const auto& [kind, list] : sources_) {
    for (const RunnerSourceInfo& info : list) {
      if (info.id == id) return QString::fromStdString(info.label);
    }
  }
  return QString();
}

void RunnersPage::RefreshInstalled() {
  MiradClient::GetConfigAsync(this, [this](ConfigResult result) {
    if (!result.ok) return;
    const auto found = result.values.find(kDefaultKey);
    default_windows_ = found != result.values.end() ? found->second : std::string();
    RebuildInstalled();
  });
  MiradClient::ListRunnersAsync(this, [this](RunnersResult result) {
    if (!result.ok) {
      SetStatus(QString::fromStdString(result.error), true);
      return;
    }
    runners_ = result.runners;
    RebuildInstalled();
  });
}

void RunnersPage::RefreshCatalog() {
  catalog_loaded_ = false;
  RebuildCatalog();
  // The one call that leaves the machine; say so rather than look hung.
  SetStatus("Checking for builds…");
  const std::string kind = CurrentKind();
  const std::string source = source_->currentData().toString().toStdString();
  MiradClient::GetRunnerCatalogAsync(this, kind, source, [this, kind, source](RunnerCatalogResult result) {
    if (kind != CurrentKind() || source != source_->currentData().toString().toStdString()) return;
    if (!result.ok) {
      SetStatus("Could not list builds: " + QString::fromStdString(result.error), true);
      return;
    }
    releases_ = result.releases;
    catalog_loaded_ = true;
    SetStatus(QString());
    RebuildCatalog();
  });
}

void RunnersPage::RebuildInstalled() {
  ClearLayout(installed_list_);
  const std::string kind = CurrentKind();
  const theme::Tokens& tokens = theme::Current();
  int shown = 0;
  for (const RunnerInfo& runner : runners_) {
    if (runner.kind != kind) continue;
    ++shown;
    const bool is_default = runner.reference == default_windows_;
    const auto used = std::ranges::count(games_, runner.reference, &GameSummary::runner_ref);

    auto* row = new QFrame();
    row->setObjectName("runner_row");
    theme::SetStyleProperty(row, "current", is_default ? "true" : "false");
    auto* row_layout = new QVBoxLayout(row);
    row_layout->setContentsMargins(14, 12, 14, 12);
    row_layout->setSpacing(6);

    auto* name_line = new QHBoxLayout();
    auto* name = new QLabel(QString::fromStdString(runner.label), row);
    name->setStyleSheet("font-weight: 600;");
    name_line->addWidget(name, /*stretch=*/1);
    if (is_default) {
      auto* badge = new QLabel("Default", row);
      badge->setStyleSheet(QString("background: %1; color: %2; border-radius: 9px; padding: 1px 9px; "
                                   "font-weight: 600;")
                               .arg(tokens.accent.name(), tokens.on_accent.name()));
      name_line->addWidget(badge);
    }
    row_layout->addLayout(name_line);

    QStringList facts;
    facts << (used == 1 ? QString("Used by 1 game") : QString("Used by %1 games").arg(used));
    // Some builds only report a bare build timestamp, which means nothing here.
    const QString version = QString::fromStdString(runner.version);
    bool numeric = false;
    version.toLongLong(&numeric);
    if (!version.isEmpty() && !numeric && runner.version != runner.name) facts << version;
    if (const QString source = SourceLabel(runner.source); !source.isEmpty()) facts << source;
    // Not Mira's to remove or update: the distro's, Steam's or the system's.
    if (!runner.removable) facts << "Managed outside Mira";
    row_layout->addWidget(Muted(facts.join(" · "), row));

    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    const auto update = std::ranges::find(updates_, runner.reference, &RunnerUpdate::reference);
    if (update != updates_.end()) {
      const DownloadTracker::Entry* entry = downloads_->Find(DownloadTracker::KeyFor(
          DownloadTracker::Kind::Runner, QString::fromStdString(kind), QString::fromStdString(update->name)));
      if (entry != nullptr && entry->state == DownloadTracker::State::Running) {
        actions->addWidget(Muted("Updating to " + QString::fromStdString(update->label) + "…", row));
      } else {
        auto* update_button = new QPushButton("Update to " + QString::fromStdString(update->label), row);
        update_button->setDefault(true);
        connect(update_button, &QPushButton::clicked, this, [this, update_button, runner, u = *update] {
          update_button->setEnabled(false);
          Update(runner, u);
        });
        actions->addWidget(update_button);
      }
    }
    if (!is_default) {
      auto* make_default = new QPushButton("Make default", row);
      connect(make_default, &QPushButton::clicked, this,
              [this, reference = runner.reference] { SetDefault(reference); });
      actions->addWidget(make_default);
    }
    if (runner.removable) {
      auto* remove = new QPushButton("Remove", row);
      remove->setObjectName("danger");
      connect(remove, &QPushButton::clicked, this, [this, runner] { Remove(runner); });
      actions->addWidget(remove);
    }
    actions->addStretch(1);
    row_layout->addLayout(actions);
    installed_list_->addWidget(row);
  }
  if (shown == 0) {
    installed_list_->addWidget(Muted(kind == "proton" ? "No Proton builds yet. Install one from Get more."
                                                      : "No Wine builds yet. Install one from Get more.",
                                     this));
  }

  const bool automatic = default_windows_.empty() || default_windows_ == "auto";
  // "proton:GE-Proton11-7" reads as "GE-Proton11-7".
  const QString chosen = QString::fromStdString(default_windows_).section(':', 1);
  default_note_->setText(
      automatic ? "Windows games set to Auto use the newest Proton build, or Wine if there's none."
                : QString("Windows games set to Auto use %1. <a href=\"auto\" style=\"color:%2\">Pick "
                          "automatically instead</a>")
                      .arg(chosen.toHtmlEscaped(), tokens.accent.name()));
}

void RunnersPage::RebuildCatalog() {
  ClearLayout(catalog_list_);
  if (!catalog_loaded_) return;
  const std::string kind = CurrentKind();
  const theme::Tokens& tokens = theme::Current();
  for (size_t i = 0; i < releases_.size(); ++i) {
    const RunnerRelease& release = releases_[i];
    const QString tag = QString::fromStdString(release.tag);

    auto* row = new QFrame();
    row->setObjectName(i + 1 < releases_.size() ? "catalog_row" : "catalog_row_last");
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(4, 10, 4, 10);
    row_layout->setSpacing(12);

    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    auto* name = new QLabel(QString::fromStdString(release.label), row);
    name->setStyleSheet("font-weight: 600;");
    text->addWidget(name);
    QStringList facts;
    if (const QString date = FormatDate(release.published_at); !date.isEmpty()) facts << date;
    if (const QString size = FormatSize(release.size_bytes); !size.isEmpty()) facts << size;
    // Installs either way; it just can't be verified first, which is worth
    // knowing before a 500 MB download.
    facts << (release.has_checksum ? "verified" : "no checksum");
    QLabel* details = Muted(facts.join(" · "), row);
    if (!release.has_checksum) details->setStyleSheet(QString("color: %1;").arg(tokens.warning.name()));
    text->addWidget(details);
    row_layout->addLayout(text, /*stretch=*/1);

    const DownloadTracker::Entry* entry = downloads_->Find(DownloadTracker::KeyFor(
        DownloadTracker::Kind::Runner, QString::fromStdString(kind), QString::fromStdString(release.name)));
    const bool downloading = entry != nullptr && entry->state == DownloadTracker::State::Running;
    if (downloading) {
      auto* progress = new QProgressBar(row);
      progress->setRange(0, 0);
      progress->setTextVisible(false);
      progress->setFixedSize(90, 4);
      row_layout->addWidget(progress);
    } else if (release.installed) {
      auto* label = Muted("Installed", row);
      label->setWordWrap(false);
      row_layout->addWidget(label);
    } else {
      auto* install = new QPushButton("Install", row);
      install->setDefault(true);
      install->setFixedWidth(90);
      connect(install, &QPushButton::clicked, this, [this, install, tag = release.tag] {
        install->setEnabled(false);  // until the tracker hears it started
        Download(tag);
      });
      row_layout->addWidget(install);
    }
    catalog_list_->addWidget(row);
  }
  if (releases_.empty()) catalog_list_->addWidget(Muted("No builds found.", this));
}

void RunnersPage::RebuildTools() {
  ClearLayout(tools_list_);
  const theme::Tokens& tokens = theme::Current();
  for (const RunnerTool& tool : tools_) {
    if (tool.installed) continue;
    // umu only matters to Proton, winetricks to both.
    if (tool.id == "umu" && CurrentKind() != "proton") continue;
    auto* row = new QFrame();
    row->setObjectName("runners_card");
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(18, 12, 18, 12);
    row_layout->setSpacing(12);
    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    auto* name = new QLabel(QString::fromStdString(tool.label) + " isn't installed", row);
    name->setStyleSheet(QString("font-weight: 600; color: %1;").arg(tokens.warning.name()));
    text->addWidget(name);
    text->addWidget(Muted(QString::fromStdString(tool.doc), row));
    row_layout->addLayout(text, /*stretch=*/1);
    const DownloadTracker::Entry* entry = downloads_->Find(
        DownloadTracker::KeyFor(DownloadTracker::Kind::Tool, QString::fromStdString(tool.id), QString()));
    if (entry != nullptr && entry->state == DownloadTracker::State::Running) {
      auto* progress = new QProgressBar(row);
      progress->setRange(0, 0);
      progress->setTextVisible(false);
      progress->setFixedSize(90, 4);
      row_layout->addWidget(progress);
    } else {
      auto* install = new QPushButton("Install", row);
      install->setDefault(true);
      connect(install, &QPushButton::clicked, this, [this, install, tool] {
        install->setEnabled(false);
        SetupTool(tool);
      });
      row_layout->addWidget(install);
    }
    tools_list_->addWidget(row);
  }
}

void RunnersPage::Update(const RunnerInfo& runner, const RunnerUpdate& update) {
  replacing_[update.name] = runner.reference;
  MiradClient::UpdateRunnerAsync(this, runner.reference, [this, name = update.name](RunnerDownloadResult result) {
    if (!result.ok) {
      replacing_.erase(name);
      SetStatus("Could not start the update: " + QString::fromStdString(result.error), true);
      RebuildInstalled();
    }
  });
}

void RunnersPage::SetupTool(const RunnerTool& tool) {
  MiradClient::SetupRunnerToolAsync(this, tool.id, [this, label = tool.label](RunnerDownloadResult result) {
    if (!result.ok) {
      SetStatus(QString("Could not install %1: %2").arg(QString::fromStdString(label), QString::fromStdString(result.error)),
                true);
      RefreshTools();
    }
  });
}

void RunnersPage::SetDefault(const std::string& reference) {
  MiradClient::PatchConfigAsync(this, {ConfigEdit{kDefaultKey, "a string", reference}},
                                [this](PatchConfigResult result) {
                                  if (!result.ok) {
                                    SetStatus("Could not set the default: " +
                                                  QString::fromStdString(result.error),
                                              true);
                                    return;
                                  }
                                  RefreshInstalled();
                                });
}

void RunnersPage::Remove(const RunnerInfo& runner) {
  const QString name = QString::fromStdString(runner.label);
  if (!notify::Confirm(this, "Remove runner", QString("Remove %1? This deletes its files.").arg(name), "Remove",
                       /*destructive=*/true)) {
    return;
  }
  MiradClient::DeleteRunnerAsync(this, runner.kind, runner.name, [this](RunnerRemoveResult result) {
    if (!result.ok) {
      SetStatus("Could not remove that runner: " + QString::fromStdString(result.error), true);
      return;
    }
    RefreshInstalled();
  });
}

void RunnersPage::Download(const std::string& tag) {
  const std::string kind = CurrentKind();
  const std::string source = source_->currentData().toString().toStdString();
  MiradClient::DownloadRunnerAsync(this, kind, tag, source, [this](RunnerDownloadResult result) {
    // The tracker hears the rest on the event stream.
    if (!result.ok) SetStatus("Could not start the download: " + QString::fromStdString(result.error), true);
  });
}

void RunnersPage::DownloadChanged(const QString& key) {
  if (key.startsWith("tool:")) {
    const DownloadTracker::Entry* entry = downloads_->Find(key);
    if (entry != nullptr && entry->state == DownloadTracker::State::Failed) {
      SetStatus(QString("Installing %1 failed: %2").arg(entry->source, entry->error), true);
    }
    if (entry != nullptr && entry->state != DownloadTracker::State::Running) {
      RefreshTools();
      RefreshInstalled();  // Proton builds need umu to be listed
    } else {
      RebuildTools();
    }
    return;
  }
  if (!key.startsWith("runner:")) return;
  const DownloadTracker::Entry* entry = downloads_->Find(key);
  if (entry == nullptr) return;
  if (entry->state == DownloadTracker::State::Failed) {
    SetStatus(QString("Downloading %1 failed: %2").arg(downloads_->NameFor(*entry), entry->error), true);
    replacing_.erase(entry->ref.toStdString());
  }
  if (entry->state == DownloadTracker::State::Finished) {
    // Discoverable once on disk; the list re-discovers each time.
    RefreshInstalled();
    RefreshUpdates();
    if (!catalog_loaded_ || std::ranges::any_of(releases_, [&](const RunnerRelease& release) {
          return QString::fromStdString(release.name) == entry->ref;
        })) {
      RefreshCatalog();
    }
    if (auto it = replacing_.find(entry->ref.toStdString()); it != replacing_.end()) {
      const std::string old_reference = it->second;
      replacing_.erase(it);
      const auto old = std::ranges::find(runners_, old_reference, &RunnerInfo::reference);
      if (old != runners_.end() &&
          notify::Confirm(this, "Update finished",
                          QString("Games that used %1 now use %2. Remove %1?")
                              .arg(QString::fromStdString(old->label), downloads_->NameFor(*entry)),
                          "Remove", /*destructive=*/true)) {
        MiradClient::DeleteRunnerAsync(this, old->kind, old->name, [this](RunnerRemoveResult result) {
          if (!result.ok) SetStatus("Could not remove the old build: " + QString::fromStdString(result.error), true);
          RefreshInstalled();
        });
      }
    }
  }
  RebuildInstalled();
  RebuildCatalog();
}

}  // namespace mira_gui
