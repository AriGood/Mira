#include "SettingsPanel.h"

#include "../dialogs/SettingsCategories.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

#include "Notify.h"
#include "SystemNotifier.h"
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <map>

namespace mira_gui {

SettingsPanel::SettingsPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(10);

  show_advanced_ = new QCheckBox("Show advanced && expert settings", this);
  connect(show_advanced_, &QCheckBox::toggled, this, &SettingsPanel::SetAdvancedVisible);
  layout->addWidget(show_advanced_);

  tabs_ = new QTabWidget(this);
  layout->addWidget(tabs_, /*stretch=*/1);
  BuildInterfaceGroup();

  setEnabled(false);
  Load();
}

QFormLayout* SettingsPanel::AddCategoryTab(const QString& title) {
  auto* page = new QWidget(this);
  auto* form = new QFormLayout(page);
  form->setVerticalSpacing(10);
  form->setHorizontalSpacing(14);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);

  auto* scroll = new QScrollArea(this);
  scroll->setWidget(page);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  tabs_->addTab(scroll, title);
  return form;
}

void SettingsPanel::BuildInterfaceGroup() {
  auto* form = AddCategoryTab("Interface");
  QWidget* box = form->parentWidget();

  scan_on_startup_ = new QCheckBox(box);
  scan_on_startup_->setChecked(true);
  scan_on_startup_->setToolTip(
      "Run a library scan when the frontend opens. mirad's own watcher keeps the library current "
      "while it runs, so this only matters for changes made while it was stopped.");
  form->addRow("Scan the library on startup", scan_on_startup_);

  notifications_ = new QComboBox(box);
  notifications_->addItem("When Mira isn't focused", "auto");
  notifications_->addItem("Always as a desktop notification", "system");
  notifications_->addItem("Always inside the window", "in_app");
  notifications_->setToolTip(
      mira_gui::notify::system_notifier::Available()
          ? "Background results — a runner finishing downloading, metadata arriving — can go to "
            "the desktop's notification service instead of a card inside the window."
          : "No desktop notification service is running, so everything is shown inside the "
            "window whatever this says.");
  form->addRow("Show background results", notifications_);

  notification_timeout_ = new QSpinBox(box);
  notification_timeout_->setRange(0, mira_gui::notify::kMaxTimeoutSeconds);
  notification_timeout_->setSuffix(" seconds");
  notification_timeout_->setSpecialValueText("Until dismissed");
  notification_timeout_->setToolTip(
      "How long a notification stays up. \"Until dismissed\" is the default.");
  form->addRow("Keep notifications for", notification_timeout_);

  game_settings_in_sidebar_ = new QCheckBox(box);
  game_settings_in_sidebar_->setChecked(true);
  game_settings_in_sidebar_->setToolTip(
      "\"Details & settings\" edits a game inline in the right panel instead of opening a "
      "separate window.");
  form->addRow("Edit a game in the sidebar", game_settings_in_sidebar_);

  auto* note = new QLabel("Stored in frontend.toml, never interpreted by the daemon.", box);
  note->setWordWrap(true);
  note->setStyleSheet("color: #9e9e9e; font-size: 11px;");
  form->addRow(note);

  LoadFrontendPrefs();
}

void SettingsPanel::LoadFrontendPrefs() {
  notifications_original_ =
      mira_gui::notify::DeliveryToString(mira_gui::notify::CurrentDelivery());
  notifications_->setCurrentIndex(notifications_->findData(notifications_original_));
  notification_timeout_original_ = mira_gui::notify::CurrentTimeoutSeconds();
  notification_timeout_->setValue(notification_timeout_original_);

  mira_gui::MiradClient::GetFrontendPrefsAsync(this, [this](mira_gui::FrontendPrefsResult result) {
    if (!result.ok) return;  // the defaults are already shown
    if (result.prefs.scan_on_startup) {
      scan_on_startup_original_ = *result.prefs.scan_on_startup;
      scan_on_startup_->setChecked(scan_on_startup_original_);
    }
    if (result.prefs.notifications) {
      notifications_original_ = QString::fromStdString(*result.prefs.notifications);
      const int index = notifications_->findData(notifications_original_);
      if (index >= 0) notifications_->setCurrentIndex(index);
    }
    if (result.prefs.notification_timeout_s) {
      notification_timeout_->setValue(*result.prefs.notification_timeout_s);
      notification_timeout_original_ = notification_timeout_->value();  // after the clamp
    }
    if (result.prefs.game_settings_in_sidebar) {
      game_settings_in_sidebar_original_ = *result.prefs.game_settings_in_sidebar;
      game_settings_in_sidebar_->setChecked(game_settings_in_sidebar_original_);
    }
  });
}

void SettingsPanel::Load() {
  mira_gui::MiradClient::GetConfigSchemaAsync(this, [this](mira_gui::ConfigSchemaResult schema) {
    if (!schema.ok) {
      emit LoadFailed(QString::fromStdString(schema.error));
      return;
    }

    for (mira_gui::ConfigSchemaEntry& entry : schema.entries) {
      fields_.push_back(Field{std::move(entry), std::string(), nullptr, nullptr, nullptr, nullptr, nullptr});
    }
    BuildRows();

    mira_gui::MiradClient::ListRunnersAsync(this, [this](mira_gui::RunnersResult result) {
      PopulateRunnerCombos(result);
    });

    mira_gui::MiradClient::GetConfigAsync(this, [this](mira_gui::ConfigResult config) {
      if (!config.ok) {
        emit LoadFailed(QString::fromStdString(config.error));
        return;
      }
      for (Field& field : fields_) {
        const auto it = config.values.find(field.entry.key);
        SetFieldText(field, it != config.values.end() ? it->second : field.entry.default_display);
        field.original = CurrentText(field);
      }
      setEnabled(true);
    });
  });
}

void SettingsPanel::BuildRows() {
  std::map<QString, std::vector<size_t>> buckets;
  for (size_t i = 0; i < fields_.size(); ++i) {
    buckets[QString::fromStdString(fields_[i].entry.category)].push_back(i);
  }

  QStringList ordered_categories;
  for (const QString& category : mira_gui::settings::CategoryOrder()) {
    if (buckets.contains(category)) ordered_categories.push_back(category);
  }
  for (const auto& [category, indices] : buckets) {
    if (!ordered_categories.contains(category)) ordered_categories.push_back(category);
  }

  for (const QString& category : ordered_categories) {
    CategoryGroup group;
    group.form = AddCategoryTab(category);
    group.tab_index = tabs_->count() - 1;

    for (const size_t i : buckets[category]) {
      Field& field = fields_[i];
      field.owner_form = group.form;
      if (field.entry.tier == "basic") group.has_basic = true;

      auto* row_widget = new QWidget(this);
      auto* row_layout = new QHBoxLayout(row_widget);
      row_layout->setContentsMargins(0, 0, 0, 0);
      row_layout->setSpacing(8);

      if (field.entry.type == "a boolean") {
        field.check = new QCheckBox(row_widget);
        row_layout->addWidget(field.check);
        row_layout->addStretch(1);
      } else if (!field.entry.one_of.empty()) {
        field.combo = new QComboBox(row_widget);
        for (const std::string& option : field.entry.one_of) {
          field.combo->addItem(QString::fromStdString(option));
        }
        row_layout->addWidget(field.combo, /*stretch=*/1);
      } else if (field.entry.minimum && field.entry.maximum &&
                 (field.entry.type == "an integer" || field.entry.type == "a number")) {
        field.spin = new QDoubleSpinBox(row_widget);
        field.spin->setDecimals(field.entry.type == "an integer" ? 0 : 2);
        field.spin->setRange(*field.entry.minimum, *field.entry.maximum);
        field.spin->setKeyboardTracking(false);
        field.spin->setToolTip(QString("Between %1 and %2")
                                   .arg(*field.entry.minimum)
                                   .arg(*field.entry.maximum));
        row_layout->addWidget(field.spin, /*stretch=*/1);
        row_layout->addStretch(1);
      } else if (field.entry.is_runner_ref) {
        field.combo = new QComboBox(row_widget);
        field.combo->setEditable(true);
        field.combo->setInsertPolicy(QComboBox::NoInsert);
        connect(field.combo, QOverload<int>::of(&QComboBox::activated), this,
                [field = &field](int idx) {
                  field->combo->setEditText(field->combo->itemData(idx).toString());
                  field->combo->lineEdit()->setCursorPosition(0);
                });
        row_layout->addWidget(field.combo, /*stretch=*/1);

        auto* reset_button = new QPushButton("Reset", row_widget);
        reset_button->setMaximumWidth(56);
        reset_button->setToolTip(
            QString("Reset to default: %1").arg(QString::fromStdString(field.entry.default_display)));
        connect(reset_button, &QPushButton::clicked, this, [this, i] { ResetField(i); });
        row_layout->addWidget(reset_button);
      } else {
        field.line = new QLineEdit(row_widget);
        if (field.entry.type == "an array of strings") {
          field.line->setPlaceholderText("comma-separated");
        }
        if (field.entry.is_secret) {
          field.line->setEchoMode(QLineEdit::PasswordEchoOnEdit);
        }
        row_layout->addWidget(field.line, /*stretch=*/1);

        auto* reset_button = new QPushButton("Reset", row_widget);
        reset_button->setMaximumWidth(56);
        reset_button->setToolTip(
            QString("Reset to default: %1").arg(QString::fromStdString(field.entry.default_display)));
        connect(reset_button, &QPushButton::clicked, this, [this, i] { ResetField(i); });
        row_layout->addWidget(reset_button);
      }

      auto* label = new QLabel(QString::fromStdString(field.entry.key), this);
      label->setToolTip(QString::fromStdString(field.entry.doc));
      row_widget->setToolTip(QString::fromStdString(field.entry.doc));

      field.row_widget = row_widget;
      group.form->addRow(label, row_widget);
    }

    groups_.push_back(group);
  }

  SetAdvancedVisible(show_advanced_->isChecked());
}

void SettingsPanel::PopulateRunnerCombos(const mira_gui::RunnersResult& result) {
  for (Field& field : fields_) {
    if (!field.combo || !field.entry.is_runner_ref) continue;

    const QString current = field.combo->currentText();
    field.combo->blockSignals(true);
    field.combo->clear();
    field.combo->addItem("Auto (best available)", "auto");
    if (result.ok) {
      for (const mira_gui::RunnerInfo& runner : result.runners) {
        const QString label = QString("%1 (%2)").arg(QString::fromStdString(runner.name),
                                                       QString::fromStdString(runner.kind));
        field.combo->addItem(label, QString::fromStdString(runner.reference));
      }
    }
    field.combo->setEditText(current);
    field.combo->blockSignals(false);
  }
}

void SettingsPanel::SetAdvancedVisible(bool show) {
  for (const Field& field : fields_) {
    if (field.entry.tier != "basic") field.owner_form->setRowVisible(field.row_widget, show);
  }
  for (const CategoryGroup& group : groups_) tabs_->setTabVisible(group.tab_index, group.has_basic || show);
}

std::string SettingsPanel::CurrentText(const Field& field) const {
  if (field.check) return field.check->isChecked() ? "true" : "false";
  if (field.spin) return field.spin->cleanText().toStdString();
  if (field.combo) return field.combo->currentText().toStdString();
  return field.line->text().toStdString();
}

void SettingsPanel::SetFieldText(Field& field, const std::string& text) {
  if (field.check) {
    field.check->setChecked(text == "true");
    return;
  }
  if (field.spin) {
    field.spin->setValue(QString::fromStdString(text).toDouble());
    return;
  }
  if (field.combo != nullptr && !field.combo->isEditable()) {
    const int index = field.combo->findText(QString::fromStdString(text));
    if (index >= 0) {
      field.combo->setCurrentIndex(index);
    } else if (!text.empty()) {
      field.combo->insertItem(0, QString::fromStdString(text));
      field.combo->setCurrentIndex(0);
    }
    return;
  }
  QLineEdit* edit = field.combo ? field.combo->lineEdit() : field.line;
  edit->setText(QString::fromStdString(text));
  edit->setCursorPosition(0);
}

void SettingsPanel::ResetField(size_t index) {
  mira_gui::MiradClient::ResetConfigKeyAsync(
      this, fields_[index].entry.key, [this, index](mira_gui::PatchConfigResult result) {
        if (!result.ok) {
          emit SaveFinished(false, QString::fromStdString(result.error));
          return;
        }
        Field& field = fields_[index];
        SetFieldText(field, field.entry.default_display);
        field.original = CurrentText(field);
      });
}

bool SettingsPanel::IsDirty() const {
  if (scan_on_startup_->isChecked() != scan_on_startup_original_) return true;
  if (notifications_->currentData().toString() != notifications_original_) return true;
  if (notification_timeout_->value() != notification_timeout_original_) return true;
  if (game_settings_in_sidebar_->isChecked() != game_settings_in_sidebar_original_) return true;
  for (const Field& field : fields_) {
    if (CurrentText(field) != field.original) return true;
  }
  return false;
}

void SettingsPanel::Save() {
  const QString notifications = notifications_->currentData().toString();
  const int timeout = notification_timeout_->value();
  const bool game_settings_in_sidebar = game_settings_in_sidebar_->isChecked();
  if (scan_on_startup_->isChecked() != scan_on_startup_original_ ||
      notifications != notifications_original_ || timeout != notification_timeout_original_ ||
      game_settings_in_sidebar != game_settings_in_sidebar_original_) {
    mira_gui::FrontendPrefs prefs;
    prefs.scan_on_startup = scan_on_startup_->isChecked();
    prefs.notifications = notifications.toStdString();
    prefs.notification_timeout_s = timeout;
    prefs.game_settings_in_sidebar = game_settings_in_sidebar;
    scan_on_startup_original_ = *prefs.scan_on_startup;
    notifications_original_ = notifications;
    notification_timeout_original_ = timeout;
    game_settings_in_sidebar_original_ = game_settings_in_sidebar;
    mira_gui::notify::SetDelivery(mira_gui::notify::DeliveryFromString(notifications));
    mira_gui::notify::SetTimeoutSeconds(timeout);
    mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
  }

  std::vector<mira_gui::ConfigEdit> edits;
  for (const Field& field : fields_) {
    const std::string current = CurrentText(field);
    if (current != field.original) {
      edits.push_back(mira_gui::ConfigEdit{field.entry.key, field.entry.type, current});
    }
  }

  if (edits.empty()) {
    emit SaveFinished(true, QString());
    return;
  }

  setEnabled(false);
  mira_gui::MiradClient::PatchConfigAsync(this, edits, [this](mira_gui::PatchConfigResult result) {
    setEnabled(true);
    if (!result.ok) {
      emit SaveFinished(false, QString::fromStdString(result.error));
      return;
    }
    for (Field& field : fields_) field.original = CurrentText(field);
    emit SaveFinished(true, QString());
  });
}

}  // namespace mira_gui
