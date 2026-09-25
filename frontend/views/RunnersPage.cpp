#include "RunnersPage.h"

#include <QButtonGroup>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstring>

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

// Wine-GE tags its releases after the Proton-GE version they track
// ("GE-Proton8-26"), so in the Wine list the asset's own name
// ("wine-lutris-GE-Proton8-26") is what says it's Wine.
QString DisplayName(const std::string& kind, const RunnerRelease& release) {
  if (kind != "wine" || release.asset_name.empty()) return QString::fromStdString(release.tag);
  QString name = QString::fromStdString(release.asset_name);
  for (const char* suffix : {".tar.xz", ".tar.gz", ".tgz", ".zip", "-x86_64"}) {
    if (name.endsWith(suffix)) name.chop(static_cast<int>(strlen(suffix)));
  }
  return name;
}

// Whether an installed build's folder ("lutris-GE-Proton8-26-x86_64")
// holds `tag`, without "GE-Proton8-2" matching "GE-Proton8-26".
bool IsBuildOf(const std::string& folder, const std::string& tag) {
  for (size_t at = folder.find(tag); at != std::string::npos; at = folder.find(tag, at + 1)) {
    const size_t end = at + tag.size();
    const bool starts = at == 0 || folder[at - 1] == '-';
    const bool ends = end == folder.size() || !std::isdigit(static_cast<unsigned char>(folder[end]));
    if (starts && ends) return true;
  }
  return false;
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
    RefreshCatalog();
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

  auto* refresh = new QPushButton("Refresh", this);
  connect(refresh, &QPushButton::clicked, this, &RunnersPage::Refresh);
  QVBoxLayout* catalog_body = nullptr;
  QFrame* catalog = Card("Get more", refresh, &catalog_body, this);
  catalog_list_ = new QVBoxLayout();
  catalog_list_->setSpacing(0);
  catalog_body->addLayout(catalog_list_);
  catalog_body->addStretch(1);
  status_ = Muted(QString(), catalog);
  catalog->layout()->addWidget(status_);
  columns->addWidget(catalog, /*stretch=*/1);

  layout->addLayout(columns, /*stretch=*/1);

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
  RefreshCatalog();
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
    RebuildCatalog();  // which builds say Installed
  });
}

void RunnersPage::RefreshCatalog() {
  catalog_loaded_ = false;
  RebuildCatalog();
  // The one call that leaves the machine; say so rather than look hung.
  SetStatus("Checking for builds…");
  const std::string kind = CurrentKind();
  MiradClient::GetRunnerCatalogAsync(this, kind, [this, kind](RunnerCatalogResult result) {
    if (kind != CurrentKind()) return;
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
    auto* name = new QLabel(QString::fromStdString(runner.name), row);
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
    row_layout->addWidget(Muted(facts.join(" · "), row));

    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    if (!is_default) {
      auto* make_default = new QPushButton("Make default", row);
      connect(make_default, &QPushButton::clicked, this,
              [this, reference = runner.reference] { SetDefault(reference); });
      actions->addWidget(make_default);
    }
    // The system's own Wine isn't Mira's to delete.
    if (runner.name != "system") {
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
    auto* name = new QLabel(DisplayName(kind, release), row);
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

    const bool installed = std::ranges::any_of(runners_, [&](const RunnerInfo& runner) {
      return runner.kind == kind && IsBuildOf(runner.name, release.tag);
    });
    const DownloadTracker::Entry* entry = downloads_->Find(
        DownloadTracker::KeyFor(DownloadTracker::Kind::Runner, QString::fromStdString(kind), tag));
    const bool downloading = entry != nullptr && entry->state == DownloadTracker::State::Running;
    if (downloading) {
      auto* progress = new QProgressBar(row);
      progress->setRange(0, 0);
      progress->setTextVisible(false);
      progress->setFixedSize(90, 4);
      row_layout->addWidget(progress);
    } else if (installed) {
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
  const QString name = QString::fromStdString(runner.name);
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
  MiradClient::DownloadRunnerAsync(this, kind, tag, [this](RunnerDownloadResult result) {
    // The tracker hears the rest on the event stream.
    if (!result.ok) SetStatus("Could not start the download: " + QString::fromStdString(result.error), true);
  });
}

void RunnersPage::DownloadChanged(const QString& key) {
  if (!key.startsWith("runner:")) return;
  const DownloadTracker::Entry* entry = downloads_->Find(key);
  if (entry == nullptr) return;
  if (entry->state == DownloadTracker::State::Failed) {
    SetStatus(QString("Downloading %1 failed: %2").arg(entry->ref, entry->error), true);
  }
  // Discoverable once on disk; the list re-discovers each time.
  if (entry->state == DownloadTracker::State::Finished) RefreshInstalled();
  RebuildCatalog();
}

}  // namespace mira_gui
