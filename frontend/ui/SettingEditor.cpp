#include "SettingEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include "HelpButton.h"

namespace mira_gui {

QWidget* SettingEditor::Build(QWidget* parent, std::function<void()> on_reset) {
  row_widget = new QWidget(parent);
  auto* row_layout = new QHBoxLayout(row_widget);
  row_layout->setContentsMargins(0, 0, 0, 0);
  row_layout->setSpacing(8);

  bool resettable = false;
  if (entry.type == "a boolean") {
    check = new QCheckBox(row_widget);
    row_layout->addWidget(check);
    row_layout->addStretch(1);
  } else if (!entry.one_of.empty()) {
    combo = new QComboBox(row_widget);
    for (const std::string& option : entry.one_of) combo->addItem(QString::fromStdString(option));
    row_layout->addWidget(combo, /*stretch=*/1);
  } else if (entry.minimum && entry.maximum && (entry.type == "an integer" || entry.type == "a number")) {
    spin = new QDoubleSpinBox(row_widget);
    spin->setDecimals(entry.type == "an integer" ? 0 : 2);
    spin->setRange(*entry.minimum, *entry.maximum);
    spin->setKeyboardTracking(false);
    spin->setToolTip(QString("Between %1 and %2").arg(*entry.minimum).arg(*entry.maximum));
    row_layout->addWidget(spin, /*stretch=*/1);
    row_layout->addStretch(1);
  } else if (entry.is_runner_ref) {
    combo = new QComboBox(row_widget);
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    QObject::connect(combo, QOverload<int>::of(&QComboBox::activated), combo, [combo = combo](int idx) {
      combo->setEditText(combo->itemData(idx).toString());
      combo->lineEdit()->setCursorPosition(0);
    });
    row_layout->addWidget(combo, /*stretch=*/1);
    resettable = true;
  } else {
    line = new QLineEdit(row_widget);
    if (entry.type == "an array of strings") line->setPlaceholderText("comma-separated");
    if (entry.is_secret) line->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    row_layout->addWidget(line, /*stretch=*/1);
    resettable = true;
  }

  // Right after the editor itself: index 0 is always the editor.
  if (!entry.link.empty()) {
    row_layout->insertWidget(1, MakeExternalLink(QString::fromStdString(entry.link), row_widget));
  }

  if (resettable && on_reset) {
    auto* reset_button = new QPushButton("Reset", row_widget);
    reset_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    reset_button->setToolTip(QString("Reset to default: %1").arg(QString::fromStdString(entry.default_display)));
    QObject::connect(reset_button, &QPushButton::clicked, row_widget, std::move(on_reset));
    row_layout->addWidget(reset_button);
  }
  return row_widget;
}

QWidget* SettingEditor::BuildLabel(QWidget* parent) const {
  const std::string& text = entry.label.empty() ? entry.key : entry.label;
  return LabelWithHelp(QString::fromStdString(text), QString::fromStdString(entry.doc), parent);
}

QString SettingEditor::SearchText() const {
  return QString::fromStdString(entry.key + ' ' + entry.label + ' ' + entry.category + ' ' + entry.doc +
                                ' ' + entry.keywords);
}

QWidget* SettingEditor::Input() const {
  if (check != nullptr) return check;
  if (combo != nullptr) return combo;
  if (spin != nullptr) return spin;
  return line;
}

std::string SettingEditor::Text() const {
  if (check) return check->isChecked() ? "true" : "false";
  if (spin) return spin->cleanText().toStdString();
  if (combo) return combo->currentText().toStdString();
  return line->text().toStdString();
}

void SettingEditor::SetText(const std::string& text) {
  if (check) {
    check->setChecked(text == "true");
    return;
  }
  if (spin) {
    spin->setValue(QString::fromStdString(text).toDouble());
    return;
  }
  if (combo != nullptr && !combo->isEditable()) {
    const int index = combo->findText(QString::fromStdString(text));
    if (index >= 0) {
      combo->setCurrentIndex(index);
    } else if (!text.empty()) {
      combo->insertItem(0, QString::fromStdString(text));
      combo->setCurrentIndex(0);
    }
    return;
  }
  QLineEdit* edit = combo ? combo->lineEdit() : line;
  edit->setText(QString::fromStdString(text));
  edit->setCursorPosition(0);
}

void FillRunnerCombo(QComboBox* combo, const RunnersResult& runners) {
  const QString current = combo->currentText();
  combo->blockSignals(true);
  combo->clear();
  combo->addItem("Auto (best available)", "auto");
  if (runners.ok) {
    for (const RunnerInfo& runner : runners.runners) {
      const QString label =
          QString("%1 (%2)").arg(QString::fromStdString(runner.name), QString::fromStdString(runner.kind));
      combo->addItem(label, QString::fromStdString(runner.reference));
    }
  }
  combo->setEditText(current);
  combo->blockSignals(false);
}

}  // namespace mira_gui
