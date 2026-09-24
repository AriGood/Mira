#include "SourcePage.h"

#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidgetItem>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "../dialogs/ItchCollectionsDialog.h"
#include "../ui/ArtworkStore.h"
#include "../ui/CoverArt.h"
#include "../ui/DownloadTracker.h"
#include "../ui/GameTileDelegate.h"
#include "../ui/Icons.h"
#include "../ui/Theme.h"
#include "../ui/TileGrid.h"

namespace mira_gui {
namespace {

// Smaller than the library's default: a page holds two grids.
const QSize kTile(150, 225);

// What differs per source, in words. Endpoints live in MiradClient.
struct SourceCopy {
  QString blurb;  // one line under the page title
  QString tool;   // stores: the helper mirad drives
  QString sign_in_steps;
  QString credential_placeholder;
  QString import_button;  // empty: no import
};

SourceCopy CopyFor(const std::string& id) {
  if (id == "steam") {
    return {"Your installed Steam games. Steam itself still installs and updates them.", "", "", "",
            "Scan Steam library"};
  }
  if (id == "epic") {
    return {"Epic Games Store, through Legendary. Games run through Mira's own Wine/Proton.",
            "Legendary",
            "Open Epic's login page and sign in, then paste the code it shows (or the whole "
            "page) here.",
            "authorizationCode, or the whole page", "Import installed games"};
  }
  if (id == "gog") {
    return {"GOG, through gogdl. Games install into your games folder.", "gogdl",
            "Open GOG's login page and sign in. It ends on a blank page: paste that page's "
            "address here.",
            "Address of the blank page, or its code", "Import installed games"};
  }
  if (id == "itch") {
    return {"itch.io, through butler.", "butler",
            "Create an API key on itch.io and paste it here. Keys don't expire.", "API key",
            "Import installed games"};
  }
  if (id == "amazon") {
    return {"Amazon Games, through nile.", "nile",
            "Open Amazon's login page and sign in. It ends on an amazon.com page: paste that "
            "page's address here.",
            "Address of the page login ends on", "Import installed games"};
  }
  if (id == "humble") {
    return {"Humble Bundle purchases, through humble-cli. Downloads land in your games folder.",
            "humble-cli",
            "Sign in to Humble Bundle in your browser, then copy the value of its "
            "_simpleauth_sess cookie (developer tools → Storage or Application → Cookies) and "
            "paste it here.",
            "_simpleauth_sess cookie value", ""};
  }
  if (id == "battlenet") {
    return {"Battle.net runs in its own Wine prefix. Games you install in it show up here.", "", "",
            "", "Import games"};
  }
  if (id == "ubisoft") {
    return {"Ubisoft Connect runs in its own Wine prefix. Games you install in it show up here.",
            "", "", "", "Import games"};
  }
  if (id == "ea") {
    return {"The EA app runs in its own Wine prefix. Games you install in it show up here.", "",
            "", "", "Import games"};
  }
  return {"Lutris's Wine and native games. Nothing is moved; they stay playable in Lutris too.",
          "", "", "", "Import Lutris games"};
}

QLabel* Text(QWidget* parent, const QString& text, const char* role = nullptr) {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(true);
  if (role != nullptr) label->setProperty("role", role);
  return label;
}

void ShowLine(QLabel* label, const QString& text, const char* role) {
  label->setProperty("role", role);
  label->style()->unpolish(label);
  label->style()->polish(label);
  label->setText(text);
  label->setVisible(!text.isEmpty());
}

// mirad's messages start lowercase; this one follows a sentence.
void ShowError(QLabel* label, const QString& what, const std::string& detail) {
  QString text = QString::fromStdString(detail);
  if (!text.isEmpty()) text[0] = text[0].toUpper();
  ShowLine(label, (what + " " + text).trimmed(), "error");
}

QString Added(int added, int updated) {
  if (added == 0 && updated == 0) return "No new games found.";
  if (added == 0) return QString("No new games; %1 updated.").arg(updated);
  return QString("Added %1 game%2.").arg(added).arg(added == 1 ? "" : "s");
}

QString Heading(const QString& text, int count) {
  return count > 0 ? QString("%1  <span style='font-weight:400; opacity:0.6'>%2</span>").arg(text).arg(count)
                   : text;
}

// The page's top: the source's own color fading into the window.
class Banner : public QWidget {
public:
  Banner(QColor color, QWidget* parent) : QWidget(parent), color_(color) {}

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    QLinearGradient gradient(rect().topLeft(), rect().bottomRight());
    QColor start = color_;
    start.setAlpha(210);
    gradient.setColorAt(0.0, start);
    gradient.setColorAt(1.0, theme::Current().window);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRoundedRect(rect(), theme::Current().radius_panel, theme::Current().radius_panel);
  }

private:
  QColor color_;
};

}  // namespace

const std::vector<SourceInfo>& AllSources() {
  using Kind = SourceInfo::Kind;
  static const std::vector<SourceInfo> sources = {
      {"steam", "Steam", Kind::Local, QColor("#2a475e")},
      {"epic", "Epic Games", Kind::Store, QColor("#4a4a4a")},
      {"gog", "GOG", Kind::Store, QColor("#86328a")},
      {"itch", "itch.io", Kind::Store, QColor("#fa5c5c")},
      {"amazon", "Amazon Games", Kind::Store, QColor("#ff9900")},
      {"humble", "Humble Bundle", Kind::Store, QColor("#cc2929")},
      {"battlenet", "Battle.net", Kind::Launcher, QColor("#148eff")},
      {"ubisoft", "Ubisoft Connect", Kind::Launcher, QColor("#0070ff")},
      {"ea", "EA app", Kind::Launcher, QColor("#ff4747")},
      {"lutris", "Lutris", Kind::Local, QColor("#f39c12")},
  };
  return sources;
}

SourcePage::SourcePage(const SourceInfo& source, ArtworkStore* artwork, DownloadTracker* downloads,
                       QWidget* parent)
    : QWidget(parent), source_(source), id_(source.id.toStdString()), artwork_(artwork), downloads_(downloads) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* content = new QWidget();
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(16, 12, 16, 16);
  layout->setSpacing(16);
  layout->addWidget(BuildBanner());
  layout->addWidget(BuildSetupCard());
  if (id_ != "humble") layout->addWidget(BuildLibrarySection());
  if (HasOwned()) layout->addWidget(BuildOwnedSection());
  layout->addStretch(1);
  scroll->setWidget(content);
  outer->addWidget(scroll);

  event_stream_.Start(this,
                      [this](std::string type, std::string data) { HandleEvent(type, data); });
  connect(downloads_, &DownloadTracker::Changed, this, [this](const QString& key) {
    if (owned_grid_ != nullptr && (key.isEmpty() || key.startsWith(source_.id + ":"))) RebuildOwnedTiles();
  });

  RefreshStatus();
  if (id_ == "steam") RefreshOwned();
}

bool SourcePage::HasImport() const { return !CopyFor(id_).import_button.isEmpty(); }

bool SourcePage::HasOwned() const { return IsStore() || id_ == "steam"; }

bool SourcePage::IsOwnGame(const GameSummary& game) const { return game.source == id_; }

QWidget* SourcePage::BuildBanner() {
  auto* banner = new Banner(source_.color, this);
  banner->setMinimumHeight(150);
  auto* layout = new QVBoxLayout(banner);
  layout->setContentsMargins(20, 14, 20, 18);

  auto* top = new QHBoxLayout();
  auto* back = new QPushButton("← Library", banner);
  back->setFlat(true);
  back->setStyleSheet("QPushButton { color: white; border: none; }");
  back->setCursor(Qt::PointingHandCursor);
  connect(back, &QPushButton::clicked, this, &SourcePage::BackRequested);
  top->addWidget(back);
  top->addStretch(1);
  filter_ = new QLineEdit(banner);
  filter_->setPlaceholderText("Filter…");
  filter_->setClearButtonEnabled(true);
  filter_->setFixedWidth(220);
  connect(filter_, &QLineEdit::textChanged, this, &SourcePage::ApplyFilter);
  top->addWidget(filter_);
  layout->addLayout(top);
  layout->addStretch(1);

  auto* bottom = new QHBoxLayout();
  auto* text = new QVBoxLayout();
  text->setSpacing(2);
  auto* title = new QLabel(source_.name, banner);
  QFont title_font = title->font();
  title_font.setPointSizeF(title_font.pointSizeF() * 2.0);
  title_font.setWeight(QFont::Bold);
  title->setFont(title_font);
  title->setStyleSheet("color: white;");
  text->addWidget(title);
  auto* blurb = new QLabel(CopyFor(id_).blurb, banner);
  blurb->setWordWrap(true);
  blurb->setStyleSheet("color: rgba(255, 255, 255, 200);");
  text->addWidget(blurb);
  status_line_ = new QLabel(banner);
  status_line_->setStyleSheet("color: rgba(255, 255, 255, 230); font-weight: 600;");
  text->addWidget(status_line_);
  bottom->addLayout(text, /*stretch=*/1);

  // Launchers: open it. Stores: sign out.
  banner_primary_ = new QPushButton(banner);
  banner_primary_->setVisible(false);
  connect(banner_primary_, &QPushButton::clicked, this, [this] {
    banner_primary_->setEnabled(false);
    if (IsLauncher()) {
      MiradClient::OpenLauncherAsync(this, id_, [this](StoreActionResult result) {
        banner_primary_->setEnabled(true);
        if (!result.ok) {
          setup_card_->setVisible(true);
          ShowError(setup_error_, "Could not open " + source_.name + ".", result.error);
        }
      });
      return;
    }
    MiradClient::SignOutStoreAsync(this, id_, [this](StoreActionResult result) {
      banner_primary_->setEnabled(true);
      if (!result.ok) {
        setup_card_->setVisible(true);
        ShowError(setup_error_, "Could not sign out.", result.error);
        return;
      }
      RefreshStatus();
    });
  });
  bottom->addWidget(banner_primary_, 0, Qt::AlignBottom);
  layout->addLayout(bottom);
  return banner;
}

QWidget* SourcePage::BuildSetupCard() {
  const SourceCopy copy = CopyFor(id_);
  setup_card_ = new QFrame(this);
  setup_card_->setObjectName("source_setup");
  const theme::Tokens& tokens = theme::Current();
  setup_card_->setStyleSheet(QString("QFrame#source_setup { background: %1; border: 1px solid %2; "
                                     "border-radius: %3px; }")
                                 .arg(tokens.surface.name(), tokens.border.name())
                                 .arg(tokens.radius_panel));
  setup_card_->setVisible(false);
  auto* layout = new QVBoxLayout(setup_card_);
  layout->setContentsMargins(18, 14, 18, 14);
  layout->setSpacing(8);

  setup_title_ = new QLabel(setup_card_);
  setup_title_->setProperty("role", "section");
  layout->addWidget(setup_title_);
  setup_text_ = Text(setup_card_, QString());
  layout->addWidget(setup_text_);

  setup_button_ = new QPushButton(setup_card_);
  setup_button_->setIcon(icons::For(icons::Glyph::Download));
  setup_button_->setVisible(false);
  connect(setup_button_, &QPushButton::clicked, this, [this] {
    setup_button_->setEnabled(false);
    setup_error_->setVisible(false);
    const auto failed = [this](StoreActionResult result) {
      if (result.ok) return;  // the event finishes the job
      launcher_installing_ = false;
      setup_button_->setEnabled(true);
      ShowError(setup_error_, "Could not start it.", result.error);
    };
    if (IsLauncher()) {
      launcher_installing_ = true;
      setup_button_->setText("Installing…");
      UpdateStatusLine();
      MiradClient::InstallLauncherAsync(this, id_, failed);
    } else {
      setup_button_->setText("Downloading…");
      MiradClient::SetupStoreToolAsync(this, id_, failed);
    }
  });
  auto* button_row = new QHBoxLayout();
  button_row->addWidget(setup_button_);
  button_row->addStretch(1);
  layout->addLayout(button_row);

  sign_in_row_ = new QWidget(setup_card_);
  sign_in_row_->setVisible(false);
  auto* sign_in_layout = new QHBoxLayout(sign_in_row_);
  sign_in_layout->setContentsMargins(0, 0, 0, 0);
  open_login_ = new QPushButton("Open login page", sign_in_row_);
  connect(open_login_, &QPushButton::clicked, this, &SourcePage::OpenLogin);
  credential_ = new QLineEdit(sign_in_row_);
  credential_->setPlaceholderText(copy.credential_placeholder);
  connect(credential_, &QLineEdit::returnPressed, this, &SourcePage::SignIn);
  sign_in_ = new QPushButton("Sign in", sign_in_row_);
  connect(sign_in_, &QPushButton::clicked, this, &SourcePage::SignIn);
  sign_in_layout->addWidget(open_login_);
  sign_in_layout->addWidget(credential_, /*stretch=*/1);
  sign_in_layout->addWidget(sign_in_);
  layout->addWidget(sign_in_row_);

  setup_error_ = Text(setup_card_, QString(), "error");
  setup_error_->setVisible(false);
  layout->addWidget(setup_error_);
  return setup_card_;
}

QWidget* SourcePage::BuildLibrarySection() {
  auto* section = new QWidget(this);
  auto* layout = new QVBoxLayout(section);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* header = new QHBoxLayout();
  library_heading_ = new QLabel("In your library", section);
  library_heading_->setProperty("role", "heading");
  header->addWidget(library_heading_);
  header->addStretch(1);
  import_result_ = Text(section, QString(), "muted");
  import_result_->setWordWrap(false);
  import_result_->setVisible(false);
  header->addWidget(import_result_);
  import_button_ = new QPushButton(CopyFor(id_).import_button, section);
  import_button_->setIcon(icons::For(icons::Glyph::Refresh));
  // Stores and launchers show it once set up (ApplyStoreStatus/ApplyLauncher).
  import_button_->setVisible(HasImport() && !IsStore() && !IsLauncher());
  connect(import_button_, &QPushButton::clicked, this, &SourcePage::Import);
  header->addWidget(import_button_);
  layout->addLayout(header);

  library_empty_ = Text(section, QString(), "muted");
  layout->addWidget(library_empty_);

  library_grid_ = new TileGrid(kTile, section);
  library_grid_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(library_grid_, &QWidget::customContextMenuRequested, this, &SourcePage::ShowLibraryMenu);
  connect(library_grid_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
    emit PlayRequested(item->data(GameTileDelegate::IdRole).toString());
  });
  layout->addWidget(library_grid_);
  return section;
}

QWidget* SourcePage::BuildOwnedSection() {
  owned_section_ = new QWidget(this);
  owned_section_->setVisible(id_ == "steam");
  auto* layout = new QVBoxLayout(owned_section_);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* header = new QHBoxLayout();
  owned_heading_ = new QLabel(id_ == "humble" ? "Your purchases" : "Not installed", owned_section_);
  owned_heading_->setProperty("role", "heading");
  header->addWidget(owned_heading_);
  header->addStretch(1);
  owned_refresh_ = new QPushButton("Refresh", owned_section_);
  owned_refresh_->setIcon(icons::For(icons::Glyph::Refresh));
  connect(owned_refresh_, &QPushButton::clicked, this, &SourcePage::RefreshOwned);
  if (id_ == "itch") {
    auto* collections = new QPushButton("Manage collections…", owned_section_);
    collections->setToolTip("Show the games from itch.io collections here, yours or any added by link.");
    connect(collections, &QPushButton::clicked, this, [this] {
      ItchCollectionsDialog dialog(this);
      dialog.exec();
      if (dialog.Changed()) RefreshOwned();
    });
    header->addWidget(collections);
  }
  header->addWidget(owned_refresh_);
  layout->addLayout(header);

  owned_note_ = Text(owned_section_, QString(), "muted");
  owned_note_->setVisible(false);
  layout->addWidget(owned_note_);
  if (id_ == "steam") {
    steam_settings_ = new QPushButton("Open Steam settings", owned_section_);
    steam_settings_->setVisible(false);
    connect(steam_settings_, &QPushButton::clicked, this,
            [this] { emit OpenSettingsRequested("steam.web_api_key"); });
    auto* row = new QHBoxLayout();
    row->addWidget(steam_settings_);
    row->addStretch(1);
    layout->addLayout(row);
  }
  if (id_ != "humble") {
    art_key_ = new QPushButton("Add a SteamGridDB key for covers", owned_section_);
    art_key_->setIcon(icons::For(icons::Glyph::Image));
    art_key_->setToolTip(source_.name + " doesn't provide covers Mira can use; SteamGridDB does, "
                         "with a free API key.");
    art_key_->setVisible(false);
    connect(art_key_, &QPushButton::clicked, this,
            [this] { emit OpenSettingsRequested("steamgriddb.api_key"); });
    auto* row = new QHBoxLayout();
    row->addWidget(art_key_);
    row->addStretch(1);
    layout->addLayout(row);
  }

  owned_grid_ = new TileGrid(kTile, owned_section_);
  owned_grid_->on_action = [this](QListWidgetItem* item) {
    const QString ref = item->data(GameTileDelegate::IdRole).toString();
    if (id_ != "humble") {
      StartInstall(ref, /*update=*/false);
      return;
    }
    owned_state_.insert(ref, "Downloading…");
    RebuildOwnedTiles();
    MiradClient::DownloadHumbleBundleAsync(this, ref.toStdString(), [this, ref](StoreActionResult r) {
      if (r.ok) return;
      owned_state_.remove(ref);
      RebuildOwnedTiles();
      ShowError(owned_note_, "Could not start the download.", r.error);
    });
  };
  layout->addWidget(owned_grid_);
  return owned_section_;
}

void SourcePage::SetGames(const std::vector<GameSummary>& games, const std::set<std::string>& running) {
  if (library_grid_ == nullptr) return;
  const QString selected = library_grid_->currentItem() != nullptr
                               ? library_grid_->currentItem()->data(GameTileDelegate::IdRole).toString()
                               : QString();
  library_grid_->clear();
  library_count_ = 0;
  for (const GameSummary& game : games) {
    if (!IsOwnGame(game)) continue;
    ++library_count_;
    auto* item = new QListWidgetItem(library_grid_);
    const QString id = QString::fromStdString(game.id);
    item->setData(GameTileDelegate::IdRole, id);
    item->setData(GameTileDelegate::NameRole, QString::fromStdString(game.name));
    item->setData(GameTileDelegate::StatusRole, QString::fromStdString(game.status));
    item->setData(GameTileDelegate::RunningRole, running.contains(game.id));
    item->setData(Qt::DecorationRole, artwork_->Cover(game, kTile, devicePixelRatioF()));
    item->setToolTip(QString::fromStdString(game.name));
    if (id == selected) library_grid_->setCurrentItem(item);
  }
  library_heading_->setText(Heading("In your library", library_count_));
  UpdateStatusLine();
  ApplyFilter();
}

void SourcePage::UpdateCover(const QString& id) {
  // A not-installed title's cover is keyed "<source>-<ref>".
  const QString prefix = source_.id + "-";
  if (owned_grid_ != nullptr && id_ != "humble" && id.startsWith(prefix)) {
    const QString ref = id.mid(prefix.size());
    for (int row = 0; row < owned_grid_->count(); ++row) {
      QListWidgetItem* item = owned_grid_->item(row);
      if (item->data(GameTileDelegate::IdRole).toString() != ref) continue;
      item->setData(Qt::DecorationRole,
                    artwork_->TitleCover(source_.id, ref, item->data(GameTileDelegate::NameRole).toString(),
                                         kTile, devicePixelRatioF()));
    }
  }
  if (library_grid_ == nullptr) return;
  for (int row = 0; row < library_grid_->count(); ++row) {
    QListWidgetItem* item = library_grid_->item(row);
    if (item->data(GameTileDelegate::IdRole).toString() != id) continue;
    GameSummary game;
    game.id = id.toStdString();
    game.name = item->data(GameTileDelegate::NameRole).toString().toStdString();
    item->setData(Qt::DecorationRole, artwork_->Cover(game, kTile, devicePixelRatioF()));
  }
}

void SourcePage::RefreshStatus() {
  if (IsStore()) {
    MiradClient::GetStoreStatusAsync(this, id_, [this](StoreStatusResult s) { ApplyStoreStatus(s); });
  } else if (IsLauncher()) {
    MiradClient::GetLaunchersAsync(this, [this](LaunchersResult result) {
      if (!result.ok) {
        setup_card_->setVisible(true);
        ShowError(setup_error_, "Could not ask mirad about " + source_.name + ".", result.error);
        return;
      }
      for (const LauncherInfo& launcher : result.launchers) {
        if (launcher.id == id_) ApplyLauncher(launcher);
      }
    });
  } else {
    UpdateStatusLine();
  }
}

void SourcePage::ApplyStoreStatus(const StoreStatusResult& status) {
  const SourceCopy copy = CopyFor(id_);
  if (!status.ok) {
    setup_card_->setVisible(true);
    setup_title_->setText(source_.name);
    ShowError(setup_error_, "Could not ask mirad about " + source_.name + ".", status.error);
    return;
  }
  const bool was_authenticated = authenticated_;
  tool_installed_ = status.tool_installed;
  authenticated_ = status.authenticated;
  account_ = status.account;
  login_url_ = status.login_url;
  setup_error_->setVisible(false);

  // One step at a time: the tool, then the account.
  setup_card_->setVisible(!tool_installed_ || !authenticated_);
  setup_button_->setVisible(!tool_installed_);
  setup_button_->setEnabled(true);
  sign_in_row_->setVisible(tool_installed_ && !authenticated_);
  if (!tool_installed_) {
    setup_title_->setText("Step 1 of 2  ·  Download " + copy.tool);
    setup_text_->setText("Mira uses " + copy.tool + " to talk to " + source_.name +
                         ". It's downloaded once, from its own releases.");
    setup_button_->setText("Download " + copy.tool);
  } else if (!authenticated_) {
    setup_title_->setText("Step 2 of 2  ·  Sign in");
    setup_text_->setText(copy.sign_in_steps);
    open_login_->setVisible(id_ == "amazon" || !login_url_.empty());
  }

  banner_primary_->setText("Sign out");
  banner_primary_->setVisible(authenticated_ && id_ != "humble");
  if (import_button_ != nullptr) import_button_->setVisible(tool_installed_ && HasImport());
  if (owned_section_ != nullptr) {
    owned_section_->setVisible(tool_installed_ && authenticated_);
    if (authenticated_ && !was_authenticated) RefreshOwned();
  }
  UpdateStatusLine();
}

void SourcePage::ApplyLauncher(const LauncherInfo& launcher) {
  launcher_installed_ = launcher.installed;
  launcher_installing_ = launcher.install_state == "running";
  setup_card_->setVisible(!launcher_installed_);
  if (!launcher_installed_) {
    setup_title_->setText("Install " + source_.name);
    setup_text_->setText(
        launcher.interactive_install
            ? "Mira makes a Wine prefix for it and runs its installer. The installer's window "
              "opens: click through it, then sign in."
            : "Mira makes a Wine prefix for it and installs it there silently. Sign in once it "
              "opens.");
    setup_button_->setVisible(true);
    setup_button_->setEnabled(!launcher_installing_);
    setup_button_->setText(launcher_installing_ ? "Installing…" : "Install " + source_.name);
    if (launcher.install_state == "failed" && !launcher.error.empty()) {
      ShowError(setup_error_, "The last install failed.", launcher.error);
    }
  }
  banner_primary_->setText("Open " + source_.name);
  banner_primary_->setVisible(launcher_installed_);
  import_button_->setVisible(launcher_installed_);
  UpdateStatusLine();
}

void SourcePage::UpdateStatusLine() {
  QStringList parts;
  if (IsStore()) {
    if (!tool_installed_) {
      parts << "Not set up";
    } else if (!authenticated_) {
      parts << "Not signed in";
    } else {
      parts << (account_.empty() ? QString("Signed in")
                                 : "Signed in as " + QString::fromStdString(account_));
    }
  } else if (IsLauncher()) {
    parts << (launcher_installing_ ? "Installing…" : launcher_installed_ ? "Installed" : "Not installed");
  }
  if (id_ != "humble") {
    parts << (library_count_ == 1 ? QString("1 game in your library")
                                  : QString("%1 games in your library").arg(library_count_));
  }
  status_line_->setText(parts.join("  ·  "));

  if (library_empty_ == nullptr) return;
  library_empty_->setVisible(library_count_ == 0);
  library_grid_->setVisible(library_count_ > 0);
  if (IsStore() && !authenticated_) {
    library_empty_->setText("Games from " + source_.name + " show up here once you're signed in.");
  } else if (IsLauncher() && !launcher_installed_) {
    library_empty_->setText("Games you install through " + source_.name + " show up here.");
  } else if (HasImport()) {
    library_empty_->setText("Nothing from " + source_.name + " in your library yet. \"" +
                            CopyFor(id_).import_button + "\" brings in what's already installed.");
  }
}

void SourcePage::OpenLogin() {
  if (id_ != "amazon") {
    QDesktopServices::openUrl(QUrl(QString::fromStdString(login_url_)));
    return;
  }
  open_login_->setEnabled(false);
  MiradClient::BeginAmazonLoginAsync(this, [this](LoginUrlResult result) {
    open_login_->setEnabled(true);
    if (!result.ok) {
      ShowError(setup_error_, "Could not start the login.", result.error);
      return;
    }
    QDesktopServices::openUrl(QUrl(QString::fromStdString(result.url)));
  });
}

void SourcePage::SignIn() {
  const QString pasted = credential_->text().trimmed();
  if (pasted.isEmpty()) return;
  sign_in_->setEnabled(false);
  sign_in_->setText("Signing in…");
  setup_error_->setVisible(false);
  MiradClient::SignInStoreAsync(this, id_, pasted.toStdString(), [this](StoreActionResult result) {
    sign_in_->setEnabled(true);
    sign_in_->setText("Sign in");
    if (!result.ok) {
      ShowError(setup_error_, "Could not sign in.", result.error);
      return;
    }
    credential_->clear();
    RefreshStatus();
  });
}

void SourcePage::Import() {
  import_button_->setEnabled(false);
  import_result_->setVisible(false);
  const auto done = [this](bool ok, const std::string& error, int added, int updated) {
    import_button_->setEnabled(true);
    if (ok) {
      ShowLine(import_result_, Added(added, updated), "muted");
    } else {
      ShowError(import_result_, "Could not import.", error);
    }
    if (ok && (added > 0 || updated > 0)) emit LibraryChanged();
  };
  if (id_ == "steam") {
    MiradClient::ScanSteamAsync(this, [done](SteamScanResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (id_ == "lutris") {
    MiradClient::ImportLutrisAsync(this, [done](LutrisImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (IsLauncher()) {
    MiradClient::ImportLauncherAsync(this, id_,
                                     [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else {
    MiradClient::ImportStoreAsync(this, id_,
                                  [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  }
}

void SourcePage::RefreshOwned() {
  owned_refresh_->setEnabled(false);
  ShowLine(owned_note_, "Loading…", "muted");
  if (id_ == "humble") {
    MiradClient::GetHumbleLibraryAsync(this, [this](HumbleLibraryResult r) { ShowBundles(r); });
  } else {
    MiradClient::GetStoreLibraryAsync(this, id_, [this](StoreLibraryResult r) { ShowOwned(r); });
  }
}

void SourcePage::ShowOwned(const StoreLibraryResult& result) {
  owned_refresh_->setEnabled(true);
  owned_.clear();
  not_owned_.clear();
  if (steam_settings_ != nullptr) steam_settings_->setVisible(false);
  if (!result.ok) {
    ShowError(owned_note_, "Could not list your games.", result.error);
    RebuildOwnedTiles();
    return;
  }
  std::vector<StoreTitle> uninstalled;
  for (const StoreTitle& title : result.titles) {
    downloads_->NoteTitle(source_.id, QString::fromStdString(title.ref), QString::fromStdString(title.title));
    if (!title.installed) {
      owned_.emplace_back(QString::fromStdString(title.ref), QString::fromStdString(title.title));
      if (!title.owned) not_owned_.insert(QString::fromStdString(title.ref));
      uninstalled.push_back(title);
    }
  }
  // Covers already fetched are skipped; the rest arrive as events.
  if (!uninstalled.empty()) {
    MiradClient::QueueTitleArtworkAsync(this, id_, std::move(uninstalled), [](StoreActionResult) {});
  }
  if (result.titles.empty() && id_ == "steam") {
    ShowLine(owned_note_,
             "Set a Steam Web API key and your SteamID64 in Settings to see the games you own "
             "but haven't installed.",
             "muted");
    if (steam_settings_ != nullptr) steam_settings_->setVisible(true);
  } else if (owned_.empty()) {
    ShowLine(owned_note_, "Everything you own is installed.", "muted");
  } else {
    ShowLine(owned_note_,
             id_ == "steam" ? "Install hands the game to the Steam client; scan the Steam library "
                              "once it's done."
                            : QString(),
             "muted");
  }
  RebuildOwnedTiles();
}

void SourcePage::ShowBundles(const HumbleLibraryResult& result) {
  owned_refresh_->setEnabled(true);
  owned_.clear();
  if (!result.ok) {
    ShowError(owned_note_, "Could not list your purchases.", result.error);
    RebuildOwnedTiles();
    return;
  }
  for (const HumbleBundle& bundle : result.bundles) {
    owned_.emplace_back(QString::fromStdString(bundle.key), QString::fromStdString(bundle.name));
    downloads_->NoteTitle("humble", QString::fromStdString(bundle.key), QString::fromStdString(bundle.name));
  }
  ShowLine(owned_note_,
           owned_.empty() ? QString("No purchases on this account.")
                          : QString("Downloads land in your games folder, where Mira picks up "
                                    "anything it recognizes."),
           "muted");
  RebuildOwnedTiles();
}

void SourcePage::RebuildOwnedTiles() {
  owned_grid_->clear();
  const QString idle = id_ == "humble" ? "Download" : "Install";
  for (const auto& [ref, title] : owned_) {
    auto* item = new QListWidgetItem(owned_grid_);
    item->setData(GameTileDelegate::IdRole, ref);
    item->setData(GameTileDelegate::NameRole, title);
    item->setData(GameTileDelegate::StatusRole, QString("ready"));
    // Bundles aren't games, so there's no cover to look up.
    item->setData(Qt::DecorationRole,
                  id_ == "humble" ? PlaceholderCover(title, source_.id + "-" + ref, kTile, devicePixelRatioF())
                                  : artwork_->TitleCover(source_.id, ref, title, kTile, devicePixelRatioF()));
    QString state = owned_state_.value(ref);
    const DownloadTracker::Entry* running = downloads_->Find(source_.id + ":" + ref);
    if (running != nullptr && running->state == DownloadTracker::State::Running) {
      state = id_ == "humble" ? "Downloading…" : running->update ? "Updating…" : "Installing…";
      if (running->progress >= 0) state = QString("%1 %2%").arg(state.chopped(1)).arg(qRound(running->progress * 100));
    }
    if (not_owned_.contains(ref)) {
      item->setData(GameTileDelegate::ActionRole, QString("Not owned"));
      item->setData(GameTileDelegate::ActionEnabledRole, false);
      item->setToolTip(title + "\nA paid game from a collection. Buy it on itch.io to install it here.");
      continue;
    }
    item->setData(GameTileDelegate::ActionRole, state.isEmpty() ? idle : state);
    item->setData(GameTileDelegate::ActionEnabledRole, state.isEmpty());
    item->setToolTip(title);
  }
  owned_heading_->setText(Heading(id_ == "humble" ? "Your purchases" : "Not installed",
                                  static_cast<int>(owned_.size())));
  ApplyFilter();
}

void SourcePage::StartInstall(const QString& ref, bool update) {
  owned_state_.insert(ref, update ? "Updating…" : "Installing…");
  if (owned_grid_ != nullptr) RebuildOwnedTiles();
  MiradClient::InstallStoreTitleAsync(this, id_, ref.toStdString(), update,
                                      [this, ref](StoreActionResult r) {
                                        if (r.ok) return;  // events report the rest
                                        owned_state_.remove(ref);
                                        if (owned_grid_ != nullptr) RebuildOwnedTiles();
                                        ShowError(import_result_, "Could not start it.", r.error);
                                      });
}

void SourcePage::ApplyFilter() {
  const QString needle = filter_->text().trimmed();
  for (TileGrid* grid : {library_grid_, owned_grid_}) {
    if (grid == nullptr) continue;
    for (int row = 0; row < grid->count(); ++row) {
      QListWidgetItem* item = grid->item(row);
      const QString name = item->data(GameTileDelegate::NameRole).toString();
      item->setHidden(!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive));
    }
    grid->FitHeight();
  }
}

void SourcePage::ShowLibraryMenu(const QPoint& pos) {
  QListWidgetItem* item = library_grid_->itemAt(pos);
  if (item == nullptr) return;
  const QString id = item->data(GameTileDelegate::IdRole).toString();
  QMenu menu(this);
  menu.addAction("Play", this, [this, id] { emit PlayRequested(id); });
  menu.addAction("Game settings…", this, [this, id] { emit OpenGameRequested(id); });
  // A store game's id is "<source>-<ref>".
  const QString prefix = source_.id + "-";
  if (IsStore() && id_ != "humble" && id.startsWith(prefix)) {
    const QString ref = id.mid(prefix.size());
    menu.addAction("Update", this, [this, ref] { StartInstall(ref, /*update=*/true); });
  }
  menu.exec(library_grid_->viewport()->mapToGlobal(pos));
}

void SourcePage::HandleEvent(const std::string& type, const std::string& data) {
  if (StoreEvent art; MiradClient::ParseTitleArtworkEvent(type, data, &art)) {
    // "ready" is LibraryWindow's: it has to land while this page is closed too.
    if (art.source == id_ && art.state == "failed" && art.error == "no_steamgriddb_key" && art_key_ != nullptr) {
      art_key_->setVisible(true);
    }
    return;
  }

  StoreEvent event;
  if (!MiradClient::ParseStoreEvent(type, data, &event) || event.source != id_) return;

  if (event.kind == "setup") {
    if (event.state == "finished") {
      launcher_installing_ = false;
      RefreshStatus();
      if (IsLauncher()) emit LibraryChanged();
    } else if (event.state == "failed") {
      launcher_installing_ = false;
      setup_button_->setEnabled(true);
      setup_button_->setText(IsLauncher() ? "Install " + source_.name : "Retry download");
      ShowError(setup_error_, "It failed.", event.error);
      UpdateStatusLine();
    }
    return;
  }

  const QString ref = QString::fromStdString(event.ref);
  if (event.state == "failed") {
    owned_state_.remove(ref);
    ShowError(owned_note_ != nullptr ? owned_note_ : import_result_, "It failed.", event.error);
  } else if (event.state == "finished") {
    if (event.kind == "download") {
      owned_state_.insert(ref, "Downloaded");
    } else if (id_ == "steam") {
      owned_state_.insert(ref, "Sent to Steam");
    } else {
      // Now a tracked game: it moves to "In your library" on the next relist.
      owned_state_.remove(ref);
      std::erase_if(owned_, [&ref](const auto& entry) { return entry.first == ref; });
      emit LibraryChanged();
    }
  } else {
    return;
  }
  if (owned_grid_ != nullptr) RebuildOwnedTiles();
}

}  // namespace mira_gui
