#include "GameDetailPageDialog.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>

#include <utility>

#include "../client/MiradClient.h"

namespace mira_gui {
namespace {

QLabel* SectionLabel(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", "section");
  return label;
}

QLabel* BodyLabel(QWidget* parent) {
  auto* label = new QLabel(parent);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

}  // namespace

GameDetailPageDialog::GameDetailPageDialog(std::string game_id, QString game_name,
                                           const std::string& runner_ref, QWidget* parent)
    : QDialog(parent), game_id_(std::move(game_id)) {
  setWindowTitle(game_name);
  resize(900, 700);

  auto* outer = new QVBoxLayout(this);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  outer->addWidget(scroll, /*stretch=*/1);

  auto* body = new QWidget(scroll);
  auto* layout = new QVBoxLayout(body);
  layout->setSpacing(12);
  scroll->setWidget(body);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  outer->addWidget(buttons);

  // A non-Steam game's cached metadata never carries these fields, so skip
  // the round trip entirely instead of fetching just to find that out.
  if (!runner_ref.starts_with("steam:")) {
    auto* empty = new QLabel(
        "Currently only displaying additional store information for Steam games.", body);
    empty->setWordWrap(true);
    empty->setProperty("role", "muted");
    layout->addWidget(empty);
    layout->addStretch(1);
    return;
  }

  auto* loading = new QLabel("Loading…", body);
  loading->setProperty("role", "muted");
  layout->addWidget(loading);
  layout->addStretch(1);

  MiradClient::GetMetadataAsync(this, game_id_, [body, layout, loading](GameMetadataResult result) {
    delete loading;

    if (!result.ok || result.missing) {
      // `missing` (a 404) is the ordinary "never fetched, or fetched and
      // found nothing" case, not a failure — result.error is only set for
      // an actual transport/server failure, and is empty here otherwise.
      const QString text = result.missing ? "No store info cached for this game yet."
                                          : QString::fromStdString(result.error);
      auto* empty = new QLabel(text, body);
      empty->setWordWrap(true);
      empty->setProperty("role", "muted");
      layout->insertWidget(0, empty);
      return;
    }

    const GameMetadata& metadata = result.metadata;
    int row = 0;

    if (!metadata.requirements_min.empty() || !metadata.requirements_rec.empty()) {
      layout->insertWidget(row++, SectionLabel("PC requirements", body));
      if (!metadata.requirements_min.empty()) {
        auto* min_label = BodyLabel(body);
        min_label->setTextFormat(Qt::RichText);
        min_label->setText(QString("<b>Minimum:</b> %1")
                               .arg(QString::fromStdString(metadata.requirements_min)));
        layout->insertWidget(row++, min_label);
      }
      if (!metadata.requirements_rec.empty()) {
        auto* rec_label = BodyLabel(body);
        rec_label->setTextFormat(Qt::RichText);
        rec_label->setText(QString("<b>Recommended:</b> %1")
                               .arg(QString::fromStdString(metadata.requirements_rec)));
        layout->insertWidget(row++, rec_label);
      }
    }

    if (!metadata.dlc_ids.empty()) {
      layout->insertWidget(row++, SectionLabel("DLC", body));
      QStringList ids;
      for (const std::int64_t id : metadata.dlc_ids) ids << QString::number(id);
      auto* dlc_label = BodyLabel(body);
      dlc_label->setText(ids.join(", "));
      layout->insertWidget(row++, dlc_label);
    }

    if (!metadata.content_descriptors.empty()) {
      layout->insertWidget(row++, SectionLabel("Content descriptors", body));
      auto* descriptors_label = BodyLabel(body);
      QString text;
      for (const std::string& descriptor : metadata.content_descriptors) {
        text += QString("• %1\n").arg(QString::fromStdString(descriptor));
      }
      descriptors_label->setText(text.trimmed());
      layout->insertWidget(row++, descriptors_label);
    }

    if (metadata.achievements_total > 0) {
      layout->insertWidget(row++, SectionLabel("Achievements", body));
      auto* achievements_label = BodyLabel(body);
      achievements_label->setText(QString("%1 achievements").arg(metadata.achievements_total));
      layout->insertWidget(row++, achievements_label);
    }

    if (!metadata.screenshots.empty()) {
      layout->insertWidget(row++, SectionLabel("Screenshots", body));
      int index = 1;
      for (const std::string& url : metadata.screenshots) {
        auto* link = BodyLabel(body);
        link->setTextFormat(Qt::RichText);
        link->setOpenExternalLinks(true);
        link->setText(QString("<a href=\"%1\">Screenshot %2</a>")
                         .arg(QString::fromStdString(url)).arg(index++));
        layout->insertWidget(row++, link);
      }
    }

    if (!metadata.trailers.empty()) {
      layout->insertWidget(row++, SectionLabel("Trailers", body));
      int index = 1;
      for (const std::string& url : metadata.trailers) {
        auto* link = BodyLabel(body);
        link->setTextFormat(Qt::RichText);
        link->setOpenExternalLinks(true);
        link->setText(QString("<a href=\"%1\">Trailer %2</a>")
                         .arg(QString::fromStdString(url)).arg(index++));
        layout->insertWidget(row++, link);
      }
    }

    if (row == 0) {
      // The caller already filtered out non-Steam games before making this
      // call, so a genuinely Steam-owned game reaching here just has none of
      // these fields cached — a sparse appdetails response, not a wrong game.
      auto* empty = new QLabel("No extra store info cached for this game yet.", body);
      empty->setWordWrap(true);
      empty->setProperty("role", "muted");
      layout->insertWidget(0, empty);
    }
  });
}

}  // namespace mira_gui
