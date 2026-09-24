#include "ManageSourcesDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "../ui/Theme.h"

namespace {

QString KindHeading(mira_gui::SourceInfo::Kind kind) {
  switch (kind) {
    case mira_gui::SourceInfo::Kind::Store:
      return "STORES";
    case mira_gui::SourceInfo::Kind::Launcher:
      return "STORE LAUNCHERS";
    case mira_gui::SourceInfo::Kind::Local:
      return "ON THIS PC";
  }
  return {};
}

}  // namespace

ManageSourcesDialog::ManageSourcesDialog(const std::vector<Entry>& entries, QWidget* parent)
    : QDialog(parent) {
  setWindowTitle("Manage sources");
  setMinimumWidth(520);
  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(4);

  auto* intro =
      new QLabel("Connect stores and launchers. Connected sources show in the sidebar.", this);
  intro->setProperty("role", "muted");
  intro->setWordWrap(true);
  layout->addWidget(intro);

  for (const auto kind : {mira_gui::SourceInfo::Kind::Store, mira_gui::SourceInfo::Kind::Launcher,
                          mira_gui::SourceInfo::Kind::Local}) {
    auto* heading = new QLabel(KindHeading(kind), this);
    heading->setProperty("role", "muted");
    heading->setStyleSheet("font-weight: 600; letter-spacing: 0.04em; margin-top: 10px;");
    layout->addWidget(heading);
    for (const Entry& entry : entries) {
      if (entry.source.kind == kind) layout->addWidget(BuildRow(entry));
    }
  }

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addSpacing(8);
  layout->addWidget(buttons);
}

QWidget* ManageSourcesDialog::BuildRow(const Entry& entry) {
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();
  auto* row = new QWidget(this);
  row->setObjectName("manage_source_row");
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(8, 6, 8, 6);
  layout->setSpacing(10);

  auto* badge = new QLabel(entry.source.name.left(1), row);
  badge->setFixedSize(30, 30);
  badge->setAlignment(Qt::AlignCenter);
  QColor fill = entry.source.color;
  if (!entry.ready) fill.setAlphaF(0.55);
  badge->setStyleSheet(
      QString("background: %1; color: white; border-radius: 7px; font-weight: 700;")
          .arg(fill.name(QColor::HexArgb)));
  layout->addWidget(badge);

  auto* text = new QVBoxLayout();
  text->setSpacing(1);
  auto* name = new QLabel(entry.source.name, row);
  name->setStyleSheet("font-weight: 600;");
  if (!entry.ready) name->setProperty("role", "muted");
  text->addWidget(name);
  auto* status = new QLabel(row);
  if (entry.ready) {
    status->setText(
        QString("Connected · %1 game%2").arg(entry.games).arg(entry.games == 1 ? "" : "s"));
    status->setStyleSheet(QString("color: %1;").arg(tokens.success.name()));
  } else {
    status->setText("Not set up");
    status->setProperty("role", "muted");
  }
  text->addWidget(status);
  layout->addLayout(text, /*stretch=*/1);

  const QString id = entry.source.id;
  if (entry.ready) {
    auto* in_sidebar = new QCheckBox("In sidebar", row);
    in_sidebar->setChecked(entry.in_sidebar);
    connect(in_sidebar, &QCheckBox::toggled, this,
            [this, id](bool on) { emit SidebarToggled(id, on); });
    layout->addWidget(in_sidebar);
  }
  auto* open = new QPushButton(entry.ready ? "Open" : "Set up", row);
  connect(open, &QPushButton::clicked, this, [this, id] {
    accept();
    emit OpenRequested(id);
  });
  layout->addWidget(open);
  return row;
}
