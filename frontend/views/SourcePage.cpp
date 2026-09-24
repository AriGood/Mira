#include "SourcePage.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "../ui/Icons.h"

namespace mira_gui {
namespace {

// What differs per source, in words. Endpoints live in MiradClient.
struct SourceCopy {
  QString tool;          // empty: no helper tool (Steam, Lutris)
  QString tool_purpose;  // "talk to Epic"
  QString sign_in_steps;
  QString credential_placeholder;
  QString login_button;
  QString import_text;  // empty: no import (Humble)
  QString import_button;
};

SourceCopy CopyFor(const std::string& id) {
  if (id == "epic") {
    return {"Legendary", "sign in to Epic and install its games",
            "Open Epic's login page and sign in. Paste the code it shows, or the whole page.",
            "authorizationCode, or the whole page", "Open Epic login",
            "Adds the Epic games Legendary already has installed.", "Import installed games"};
  }
  if (id == "gog") {
    return {"gogdl", "sign in to GOG and install its games",
            "Open GOG's login page and sign in. It ends on a blank page: paste that page's "
            "address here.",
            "Address of the blank page, or its code", "Open GOG login",
            "Adds the games already in the GOG install folder.", "Import installed games"};
  }
  if (id == "itch") {
    return {"butler", "sign in to itch.io and install its games",
            "Create an API key on itch.io and paste it here. Keys don't expire.", "API key",
            "Open itch.io API keys", "Adds the games butler has installed.",
            "Import installed games"};
  }
  if (id == "humble") {
    return {"humble-cli", "list and download your Humble Bundle purchases",
            "Sign in to Humble Bundle in your browser, then copy the value of its "
            "_simpleauth_sess cookie (in the browser's developer tools, under Storage or "
            "Application → Cookies) and paste it here.",
            "_simpleauth_sess cookie value", "Open Humble login", "", ""};
  }
  if (id == "steam") {
    return {"", "", "", "", "", "Adds your installed Steam games. Steam still updates them.",
            "Scan Steam library"};
  }
  return {"", "", "", "", "",
          "Adds Lutris's Wine and native games. Nothing is moved; they stay playable in Lutris "
          "too.",
          "Import Lutris games"};
}

QLabel* Text(QWidget* parent, const QString& text, const char* role = nullptr) {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  if (role != nullptr) label->setProperty("role", role);
  return label;
}

// A titled block; `body` receives its layout.
QWidget* Section(QWidget* parent, const QString& title, QVBoxLayout** body) {
  auto* section = new QWidget(parent);
  auto* layout = new QVBoxLayout(section);
  layout->setContentsMargins(0, 0, 0, 18);
  layout->setSpacing(8);
  auto* heading = new QLabel(title, section);
  heading->setProperty("role", "section");
  layout->addWidget(heading);
  *body = layout;
  return section;
}

QLabel* ErrorLine(QWidget* parent) {
  QLabel* label = Text(parent, QString(), "error");
  label->setVisible(false);
  return label;
}

void ShowLine(QLabel* label, const QString& text, const char* role) {
  label->setProperty("role", role);
  label->style()->unpolish(label);
  label->style()->polish(label);
  label->setText(text);
  label->setVisible(true);
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

}  // namespace

const std::vector<SourceInfo>& AllSources() {
  static const std::vector<SourceInfo> sources = {
      {"steam", "Steam"},  {"epic", "Epic Games"},       {"gog", "GOG"},
      {"itch", "itch.io"}, {"humble", "Humble Bundle"}, {"lutris", "Lutris"},
  };
  return sources;
}

SourcePage::SourcePage(const SourceInfo& source, QWidget* parent)
    : QWidget(parent), source_(source), id_(source.id.toStdString()) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 12, 16, 16);
  layout->setSpacing(10);

  auto* header = new QHBoxLayout();
  auto* back = new QPushButton("← Back", this);
  connect(back, &QPushButton::clicked, this, &SourcePage::BackRequested);
  auto* title = new QLabel(source_.name, this);
  title->setProperty("role", "heading");
  header->addWidget(back);
  header->addWidget(title);
  header->addStretch(1);
  layout->addLayout(header);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* content = new QWidget();
  auto* sections = new QVBoxLayout(content);
  sections->setContentsMargins(4, 8, 12, 0);
  sections->setSpacing(0);
  if (IsStore()) {
    sections->addWidget(BuildHelperSection());
    sections->addWidget(BuildAccountSection());
  }
  if (!CopyFor(id_).import_text.isEmpty()) sections->addWidget(BuildImportSection());
  if (HasOwnedList()) sections->addWidget(BuildOwnedSection());
  sections->addStretch(1);
  scroll->setWidget(content);
  layout->addWidget(scroll, /*stretch=*/1);

  event_stream_.Start(this,
                      [this](std::string type, std::string data) { HandleEvent(type, data); });

  if (IsStore()) {
    RefreshStatus();
  } else if (HasOwnedList()) {
    RefreshOwned();
  }
}

bool SourcePage::IsStore() const {
  return id_ == "epic" || id_ == "gog" || id_ == "itch" || id_ == "humble";
}

bool SourcePage::HasOwnedList() const { return id_ != "lutris"; }

QWidget* SourcePage::BuildHelperSection() {
  const SourceCopy copy = CopyFor(id_);
  QVBoxLayout* body = nullptr;
  helper_section_ = Section(this, copy.tool, &body);

  helper_text_ = Text(helper_section_, "Checking…", "muted");
  body->addWidget(helper_text_);

  setup_button_ = new QPushButton(helper_section_);
  setup_button_->setIcon(icons::For(icons::Glyph::Download));
  setup_button_->setVisible(false);
  connect(setup_button_, &QPushButton::clicked, this, [this] {
    setup_button_->setEnabled(false);
    setup_button_->setText("Downloading…");
    helper_error_->setVisible(false);
    MiradClient::SetupStoreToolAsync(this, id_, [this](StoreActionResult result) {
      if (result.ok) return;  // the setup event finishes the job
      setup_button_->setEnabled(true);
      setup_button_->setText("Retry download");
      ShowError(helper_error_, "Could not start the download.", result.error);
    });
  });
  auto* row = new QHBoxLayout();
  row->addWidget(setup_button_);
  row->addStretch(1);
  body->addLayout(row);

  helper_error_ = ErrorLine(helper_section_);
  body->addWidget(helper_error_);
  return helper_section_;
}

QWidget* SourcePage::BuildAccountSection() {
  const SourceCopy copy = CopyFor(id_);
  QVBoxLayout* body = nullptr;
  account_section_ = Section(this, "Account", &body);
  account_section_->setVisible(false);

  account_text_ = Text(account_section_, QString());
  body->addWidget(account_text_);

  sign_in_row_ = new QWidget(account_section_);
  auto* sign_in_layout = new QHBoxLayout(sign_in_row_);
  sign_in_layout->setContentsMargins(0, 0, 0, 0);
  open_login_ = new QPushButton(copy.login_button, sign_in_row_);
  connect(open_login_, &QPushButton::clicked, this, [this] {
    QDesktopServices::openUrl(QUrl(QString::fromStdString(login_url_)));
  });
  credential_ = new QLineEdit(sign_in_row_);
  credential_->setPlaceholderText(copy.credential_placeholder);
  connect(credential_, &QLineEdit::returnPressed, this, &SourcePage::SignIn);
  sign_in_ = new QPushButton("Sign in", sign_in_row_);
  connect(sign_in_, &QPushButton::clicked, this, &SourcePage::SignIn);
  sign_in_layout->addWidget(open_login_);
  sign_in_layout->addWidget(credential_, /*stretch=*/1);
  sign_in_layout->addWidget(sign_in_);
  body->addWidget(sign_in_row_);

  // humble-cli has no sign-out of its own.
  sign_out_ = new QPushButton("Sign out", account_section_);
  sign_out_->setVisible(false);
  connect(sign_out_, &QPushButton::clicked, this, [this] {
    sign_out_->setEnabled(false);
    MiradClient::SignOutStoreAsync(this, id_, [this](StoreActionResult result) {
      sign_out_->setEnabled(true);
      if (!result.ok) {
        ShowError(account_error_, "Could not sign out.", result.error);
        return;
      }
      RefreshStatus();
    });
  });
  auto* sign_out_row = new QHBoxLayout();
  sign_out_row->addWidget(sign_out_);
  sign_out_row->addStretch(1);
  body->addLayout(sign_out_row);

  account_error_ = ErrorLine(account_section_);
  body->addWidget(account_error_);
  return account_section_;
}

QWidget* SourcePage::BuildImportSection() {
  const SourceCopy copy = CopyFor(id_);
  QVBoxLayout* body = nullptr;
  import_section_ = Section(this, "Installed games", &body);
  // A store's import needs its helper; ApplyStatus shows it.
  import_section_->setVisible(!IsStore());

  body->addWidget(Text(import_section_, copy.import_text, "muted"));
  import_button_ = new QPushButton(copy.import_button, import_section_);
  connect(import_button_, &QPushButton::clicked, this, &SourcePage::Import);
  auto* row = new QHBoxLayout();
  row->addWidget(import_button_);
  row->addStretch(1);
  body->addLayout(row);
  import_result_ = Text(import_section_, QString());
  import_result_->setVisible(false);
  body->addWidget(import_result_);
  return import_section_;
}

QWidget* SourcePage::BuildOwnedSection() {
  QVBoxLayout* body = nullptr;
  owned_section_ = Section(this, id_ == "humble" ? "Purchases" : "Owned games", &body);
  owned_section_->setVisible(!IsStore());

  owned_note_ = Text(owned_section_, QString(), "muted");
  owned_note_->setVisible(false);
  body->addWidget(owned_note_);

  if (id_ == "steam") {
    auto* settings = new QPushButton("Open Steam settings", owned_section_);
    settings->setVisible(false);
    connect(settings, &QPushButton::clicked, this,
            [this] { emit OpenSettingsRequested("steam.web_api_key"); });
    settings->setObjectName("steam_settings");
    auto* row = new QHBoxLayout();
    row->addWidget(settings);
    row->addStretch(1);
    body->addLayout(row);
  }

  auto* tools = new QHBoxLayout();
  owned_filter_ = new QLineEdit(owned_section_);
  owned_filter_->setPlaceholderText("Filter…");
  owned_filter_->setClearButtonEnabled(true);
  connect(owned_filter_, &QLineEdit::textChanged, this, &SourcePage::FilterOwned);
  owned_refresh_ = new QPushButton("Refresh", owned_section_);
  owned_refresh_->setIcon(icons::For(icons::Glyph::Refresh));
  connect(owned_refresh_, &QPushButton::clicked, this, &SourcePage::RefreshOwned);
  tools->addWidget(owned_filter_, /*stretch=*/1);
  tools->addWidget(owned_refresh_, 0, Qt::AlignLeft);
  body->addLayout(tools);

  owned_table_ = new QTableWidget(0, 2, owned_section_);
  owned_table_->horizontalHeader()->setVisible(false);
  owned_table_->verticalHeader()->setVisible(false);
  owned_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  owned_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  owned_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  owned_table_->setSelectionMode(QAbstractItemView::NoSelection);
  owned_table_->setFocusPolicy(Qt::NoFocus);
  owned_table_->setShowGrid(false);
  owned_table_->setAlternatingRowColors(true);
  owned_table_->setMinimumHeight(320);
  owned_table_->setVisible(false);
  owned_filter_->setVisible(false);
  body->addWidget(owned_table_);
  return owned_section_;
}

void SourcePage::RefreshStatus() {
  MiradClient::GetStoreStatusAsync(this, id_, [this](StoreStatusResult status) {
    ApplyStatus(status);
  });
}

void SourcePage::ApplyStatus(const StoreStatusResult& status) {
  const SourceCopy copy = CopyFor(id_);
  if (!status.ok) {
    helper_text_->setText("Could not ask mirad about " + source_.name + ".");
    ShowError(helper_error_, QString(), status.error);
    return;
  }
  helper_error_->setVisible(false);
  const bool was_authenticated = authenticated_;
  tool_installed_ = status.tool_installed;
  authenticated_ = status.authenticated;
  login_url_ = status.login_url;

  if (tool_installed_) {
    const QString version = QString::fromStdString(status.tool_version);
    helper_text_->setText(version.isEmpty() ? copy.tool + " is installed."
                                            : copy.tool + " is installed (" + version + ").");
    setup_button_->setText("Update " + copy.tool);
  } else {
    helper_text_->setText("Mira uses " + copy.tool + " to " + copy.tool_purpose +
                          ". It isn't installed yet.");
    setup_button_->setText("Download " + copy.tool);
  }
  setup_button_->setEnabled(true);
  setup_button_->setVisible(true);

  account_section_->setVisible(tool_installed_);
  account_error_->setVisible(false);
  if (authenticated_) {
    account_text_->setText(status.account.empty()
                               ? "Signed in."
                               : "Signed in as " + QString::fromStdString(status.account) + ".");
  } else {
    account_text_->setText(copy.sign_in_steps);
  }
  sign_in_row_->setVisible(!authenticated_);
  open_login_->setVisible(!login_url_.empty());
  sign_out_->setVisible(authenticated_ && id_ != "humble");

  if (import_section_ != nullptr) import_section_->setVisible(tool_installed_);
  if (owned_section_ != nullptr) {
    owned_section_->setVisible(tool_installed_ && authenticated_);
    if (authenticated_ && (!was_authenticated || owned_table_->rowCount() == 0)) RefreshOwned();
  }
}

void SourcePage::SignIn() {
  const QString pasted = credential_->text().trimmed();
  if (pasted.isEmpty()) return;
  sign_in_->setEnabled(false);
  sign_in_->setText("Signing in…");
  account_error_->setVisible(false);
  MiradClient::SignInStoreAsync(this, id_, pasted.toStdString(), [this](StoreActionResult result) {
    sign_in_->setEnabled(true);
    sign_in_->setText("Sign in");
    if (!result.ok) {
      ShowError(account_error_, "Could not sign in.", result.error);
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
    MiradClient::ImportLutrisAsync(
        this, [done](LutrisImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else {
    MiradClient::ImportStoreAsync(
        this, id_, [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
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
  owned_table_->setRowCount(0);
  owned_installed_.clear();
  auto* steam_settings = findChild<QPushButton*>("steam_settings");
  if (steam_settings != nullptr) steam_settings->setVisible(false);
  if (!result.ok) {
    ShowError(owned_note_, "Could not list your games.", result.error);
    return;
  }
  owned_table_->setVisible(!result.titles.empty());
  owned_filter_->setVisible(!result.titles.empty());
  if (result.titles.empty()) {
    if (id_ == "steam") {
      ShowLine(owned_note_,
               "Set a Steam Web API key and your SteamID64 in Settings to list games you own "
               "but haven't installed.",
               "muted");
      if (steam_settings != nullptr) steam_settings->setVisible(true);
    } else {
      ShowLine(owned_note_, "Nothing owned on this account yet.", "muted");
    }
    return;
  }
  ShowLine(owned_note_,
           id_ == "steam" ? "Install hands the game to the Steam client. Scan the Steam library "
                            "once it's done."
                          : QString("%1 games.").arg(result.titles.size()),
           "muted");

  owned_table_->setRowCount(static_cast<int>(result.titles.size()));
  int row = 0;
  for (const StoreTitle& title : result.titles) {
    const QString ref = QString::fromStdString(title.ref);
    auto* name = new QTableWidgetItem(QString::fromStdString(title.title));
    name->setData(Qt::UserRole, ref);
    owned_table_->setItem(row, 0, name);
    owned_installed_.insert(ref, title.installed);

    auto* button = new QPushButton(owned_table_);
    button->setObjectName(ref);
    const bool can_update = id_ != "steam";
    if (title.installed) {
      button->setText(can_update ? "Update" : "Installed");
      button->setEnabled(can_update);
    } else {
      button->setText("Install");
    }
    connect(button, &QPushButton::clicked, this, [this, button, ref] {
      const bool update = owned_installed_.value(ref);
      button->setEnabled(false);
      button->setText(update ? "Updating…" : "Installing…");
      MiradClient::InstallStoreTitleAsync(this, id_, ref.toStdString(), update,
                                          [this, button, update](StoreActionResult r) {
                                            if (r.ok) return;  // events report the rest
                                            button->setEnabled(true);
                                            button->setText(update ? "Update" : "Install");
                                            ShowError(owned_note_, "Could not start it.", r.error);
                                          });
    });
    owned_table_->setCellWidget(row, 1, button);
    ++row;
  }
  FilterOwned();
}

void SourcePage::ShowBundles(const HumbleLibraryResult& result) {
  owned_refresh_->setEnabled(true);
  owned_table_->setRowCount(0);
  if (!result.ok) {
    ShowError(owned_note_, "Could not list your purchases.", result.error);
    return;
  }
  ShowLine(owned_note_,
           "Downloads land in the Humble download folder. Extract or install one into a library "
           "folder, or add it with Add game manually.",
           "muted");
  owned_table_->setVisible(!result.bundles.empty());
  owned_filter_->setVisible(!result.bundles.empty());
  owned_table_->setRowCount(static_cast<int>(result.bundles.size()));
  int row = 0;
  for (const HumbleBundle& bundle : result.bundles) {
    const QString key = QString::fromStdString(bundle.key);
    auto* name = new QTableWidgetItem(QString::fromStdString(bundle.name));
    name->setData(Qt::UserRole, key);
    owned_table_->setItem(row, 0, name);
    auto* button = new QPushButton("Download", owned_table_);
    button->setObjectName(key);
    connect(button, &QPushButton::clicked, this, [this, button, key] {
      button->setEnabled(false);
      button->setText("Downloading…");
      MiradClient::DownloadHumbleBundleAsync(this, key.toStdString(),
                                             [this, button](StoreActionResult r) {
                                               if (r.ok) return;
                                               button->setEnabled(true);
                                               button->setText("Download");
                                               ShowError(owned_note_, "Could not start it.",
                                                         r.error);
                                             });
    });
    owned_table_->setCellWidget(row, 1, button);
    ++row;
  }
  FilterOwned();
}

void SourcePage::FilterOwned() {
  const QString needle = owned_filter_->text().trimmed();
  for (int row = 0; row < owned_table_->rowCount(); ++row) {
    const QTableWidgetItem* name = owned_table_->item(row, 0);
    const bool match = needle.isEmpty() || (name != nullptr && name->text().contains(needle, Qt::CaseInsensitive));
    owned_table_->setRowHidden(row, !match);
  }
}

QPushButton* SourcePage::OwnedButton(const std::string& ref) const {
  if (owned_table_ == nullptr) return nullptr;
  return owned_table_->findChild<QPushButton*>(QString::fromStdString(ref));
}

void SourcePage::HandleEvent(const std::string& type, const std::string& data) {
  StoreEvent event;
  if (!MiradClient::ParseStoreEvent(type, data, &event) || event.source != id_) return;

  if (event.kind == "setup") {
    if (event.state == "finished") {
      RefreshStatus();
    } else if (event.state == "failed") {
      setup_button_->setEnabled(true);
      setup_button_->setText("Retry download");
      ShowError(helper_error_, "The download failed.", event.error);
    }
    return;
  }

  QPushButton* button = OwnedButton(event.ref);
  if (button == nullptr) return;
  const QString ref = QString::fromStdString(event.ref);
  if (event.state == "failed") {
    button->setEnabled(true);
    button->setText(event.kind == "download" ? "Download"
                                             : (owned_installed_.value(ref) ? "Update" : "Install"));
    ShowError(owned_note_, "It failed.", event.error);
    return;
  }
  if (event.state != "finished") return;
  if (event.kind == "download") {
    button->setText("Downloaded");
    return;
  }
  owned_installed_.insert(ref, true);
  button->setEnabled(id_ != "steam");
  button->setText(id_ == "steam" ? "Sent to Steam" : "Update");
  emit LibraryChanged();
}

}  // namespace mira_gui
