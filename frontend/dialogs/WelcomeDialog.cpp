#include "WelcomeDialog.h"

#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

#include <functional>
#include <utility>

#include "../client/MiradClient.h"

namespace mira_gui {
namespace {

QString Expanded(const QString& path) {
  if (path == "~" || path.startsWith("~/")) return QDir::homePath() + path.mid(1);
  return path;
}

QString Tilde(const QString& path) {
  const QString home = QDir::homePath();
  if (path == home) return "~";
  if (path.startsWith(home + "/")) return "~" + path.mid(home.size());
  return path;
}

QLabel* Text(QWidget* parent, const QString& text, const char* role = nullptr) {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(true);
  if (role != nullptr) label->setProperty("role", role);
  return label;
}

}  // namespace

WelcomeDialog::WelcomeDialog(const QString& folder, QWidget* parent) : QDialog(parent) {
  setWindowTitle("Welcome to Mira");
  setMinimumWidth(620);

  auto* layout = new QVBoxLayout(this);
  pages_ = new QStackedWidget(this);
  pages_->addWidget(BuildFolderPage());
  pages_->addWidget(BuildImportPage());
  layout->addWidget(pages_);

  folder_->setText(folder.isEmpty() ? "~/Games" : folder);
  UpdateFolderNote();
}

QWidget* WelcomeDialog::BuildFolderPage() {
  auto* page = new QWidget(this);
  auto* layout = new QVBoxLayout(page);
  layout->setSpacing(10);

  layout->addWidget(Text(page, "Welcome to Mira", "heading"));
  layout->addWidget(Text(page,
                         "Mira takes over one folder for your games and watches it: put a "
                         "game's folder in it and the game shows up in your library."));

  auto* row = new QHBoxLayout();
  folder_ = new QLineEdit(page);
  connect(folder_, &QLineEdit::textChanged, this, &WelcomeDialog::UpdateFolderNote);
  auto* browse = new QPushButton("Change…", page);
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString picked =
        QFileDialog::getExistingDirectory(this, "Games folder", Expanded(folder_->text().trimmed()));
    if (!picked.isEmpty()) folder_->setText(Tilde(picked));
  });
  row->addWidget(folder_, /*stretch=*/1);
  row->addWidget(browse);
  layout->addLayout(row);

  folder_note_ = Text(page, QString(), "muted");
  layout->addWidget(folder_note_);

  layout->addWidget(Text(page,
                         "Mira also keeps these inside it:\n"
                         "•  prefixes — the Wine/Proton setup for each Windows game\n"
                         "•  GOG and itch — games you install from those stores\n"
                         "•  Humble Bundle — your Humble downloads",
                         "muted"));

  folder_error_ = Text(page, QString(), "error");
  folder_error_->setVisible(false);
  layout->addWidget(folder_error_);

  layout->addStretch(1);
  auto* buttons = new QHBoxLayout();
  buttons->addStretch(1);
  use_folder_ = new QPushButton("Use this folder", page);
  use_folder_->setDefault(true);
  connect(use_folder_, &QPushButton::clicked, this, &WelcomeDialog::UseFolder);
  buttons->addWidget(use_folder_);
  layout->addLayout(buttons);
  return page;
}

QWidget* WelcomeDialog::BuildImportPage() {
  auto* page = new QWidget(this);
  auto* layout = new QVBoxLayout(page);
  layout->setSpacing(10);

  layout->addWidget(Text(page, "Bring in the games you already have", "heading"));

  // Steam and Lutris import in one click; a result line replaces the button.
  const auto import_row = [this, page, layout](const QString& text, const QString& button_text,
                                                auto start) {
    auto* row = new QHBoxLayout();
    row->addWidget(Text(page, text), /*stretch=*/1);
    auto* button = new QPushButton(button_text, page);
    auto* result = Text(page, QString(), "muted");
    result->setVisible(false);
    connect(button, &QPushButton::clicked, this, [this, button, result, start] {
      button->setEnabled(false);
      start([this, button, result](bool ok, const std::string& error, int added) {
        button->setVisible(false);
        result->setProperty("role", ok ? "muted" : "error");
        result->style()->unpolish(result);
        result->style()->polish(result);
        result->setText(ok ? (added == 0 ? QString("No new games found.")
                                         : QString("Added %1 game%2.").arg(added).arg(added == 1 ? "" : "s"))
                           : QString::fromStdString(error));
        result->setVisible(true);
        if (ok && added > 0) emit LibraryChanged();
      });
    });
    row->addWidget(button);
    row->addWidget(result);
    layout->addLayout(row);
  };
  using Done = std::function<void(bool, const std::string&, int)>;
  import_row("Steam games installed on this computer.", "Scan Steam library", [this](Done done) {
    MiradClient::ScanSteamAsync(this, [done](SteamScanResult r) { done(r.ok, r.error, r.added); });
  });
  import_row("Lutris's Wine and native games. They stay playable in Lutris too.",
             "Import Lutris games", [this](Done done) {
               MiradClient::ImportLutrisAsync(
                   this, [done](LutrisImportResult r) { done(r.ok, r.error, r.added); });
             });

  layout->addSpacing(6);
  layout->addWidget(Text(page,
                         "Sign in to a store to import its installed games and install the "
                         "ones you own. Each one is also under Sources in the sidebar."));
  auto* stores = new QHBoxLayout();
  for (const auto& [id, name] : {std::pair<QString, QString>{"epic", "Epic Games"},
                                 {"gog", "GOG"},
                                 {"itch", "itch.io"},
                                 {"humble", "Humble Bundle"}}) {
    auto* button = new QPushButton(name, page);
    connect(button, &QPushButton::clicked, this, [this, id] {
      accept();
      emit OpenSourceRequested(id);
    });
    stores->addWidget(button);
  }
  stores->addStretch(1);
  layout->addLayout(stores);

  layout->addStretch(1);
  auto* buttons = new QHBoxLayout();
  buttons->addStretch(1);
  auto* done = new QPushButton("Done", page);
  done->setDefault(true);
  connect(done, &QPushButton::clicked, this, &QDialog::accept);
  buttons->addWidget(done);
  layout->addLayout(buttons);
  return page;
}

void WelcomeDialog::UpdateFolderNote() {
  const QString path = folder_->text().trimmed();
  const QDir dir(Expanded(path));
  if (path.isEmpty()) {
    folder_note_->setText("Choose a folder.");
  } else if (!dir.exists()) {
    folder_note_->setText("Mira will create this folder.");
  } else {
    const qsizetype count = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
    folder_note_->setText(
        count == 0 ? QString("This folder is empty.")
                   : QString("This folder already has %1 item%2. Mira adds any games it finds "
                             "there; nothing is moved or deleted.")
                         .arg(count)
                         .arg(count == 1 ? "" : "s"));
  }
  use_folder_->setEnabled(!path.isEmpty());
}

void WelcomeDialog::UseFolder() {
  use_folder_->setEnabled(false);
  folder_error_->setVisible(false);
  MiradClient::SetGamesFolderAsync(this, folder_->text().trimmed().toStdString(),
                                   [this](GamesFolderResult result) {
                                     use_folder_->setEnabled(true);
                                     if (!result.ok) {
                                       folder_error_->setText("Could not use this folder: " +
                                                              QString::fromStdString(result.error));
                                       folder_error_->setVisible(true);
                                       return;
                                     }
                                     emit GamesFolderChanged();
                                     pages_->setCurrentIndex(1);
                                   });
}

}  // namespace mira_gui
