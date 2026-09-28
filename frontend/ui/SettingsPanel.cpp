#include "SettingsPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

#include "../views/SourcePage.h"
#include "HelpButton.h"
#include "KeyBindings.h"
#include "Notify.h"
#include "SettingsNav.h"
#include "Theme.h"
#include "SystemNotifier.h"
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <optional>
#include <utility>

namespace mira_gui {

SettingsPanel::SettingsPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  nav_ = new SettingsNavWidget(this);
  layout->addWidget(nav_, /*stretch=*/1);

  BuildInterfaceGroup();
  BuildSidebarGroup();
  BuildShortcutsGroup();

  setEnabled(false);
  Load();
}

void SettingsPanel::BuildSidebarGroup() {
  auto* form = nav_->AddCategory("Sidebar");
  QWidget* box = form->parentWidget();

  recent_count_ = new QSpinBox(box);
  recent_count_->setRange(0, 10);
  recent_count_->setValue(recent_count_original_);
  recent_count_->setSpecialValueText("Off");
  form->addRow("Recently Played Count", recent_count_);
  nav_->RegisterRow(form, recent_count_, "sidebar recently played recent games count");

  source_counts_ = new QCheckBox(box);
  source_counts_->setChecked(source_counts_original_);
  form->addRow("Show Game Count per Source", source_counts_);
  nav_->RegisterRow(form, source_counts_, "sidebar source game counts number");

  nav_->AddSubheading(form, "Sources");
  auto* sources_note = new QLabel("Only sources that are set up appear in the sidebar.", box);
  sources_note->setWordWrap(true);
  sources_note->setProperty("role", "muted");
  form->addRow(sources_note);
  for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
    auto* check = new QCheckBox(box);
    check->setChecked(true);
    form->addRow(QString("Show %1").arg(source.name), check);
    nav_->RegisterRow(form, check, QString("sidebar show source %1").arg(source.name));
    source_checks_.emplace_back(source.id, check);
  }
}

QSet<QString> SettingsPanel::CurrentHiddenSources() const {
  QSet<QString> hidden;
  for (const auto& [id, check] : source_checks_) {
    if (!check->isChecked()) hidden.insert(id);
  }
  return hidden;
}

bool SettingsPanel::SidebarDirty() const {
  return recent_count_->value() != recent_count_original_ ||
         source_counts_->isChecked() != source_counts_original_ ||
         CurrentHiddenSources() != hidden_sources_original_;
}

void SettingsPanel::BuildInterfaceGroup() {
  auto* form = nav_->AddCategory("Interface");
  QWidget* box = form->parentWidget();

  scan_on_startup_ = new QCheckBox(box);
  scan_on_startup_->setChecked(true);
  form->addRow(LabelWithHelp("Scan Library on Startup",
                             "Scan the library each time Mira opens. The daemon already watches your "
                             "folders while it runs, so this only catches changes made while it was "
                             "stopped.",
                             box),
               scan_on_startup_);
  nav_->RegisterRow(form, scan_on_startup_, "scan the library on startup");

  theme_ = new QComboBox(box);
  theme_->addItem("Follow the desktop", "auto");
  for (const QString& name : mira_gui::theme::Available()) theme_->addItem(name, name);
  form->addRow(LabelWithHelp("Theme",
                             "To add a theme, put a .toml file in ~/.config/mira/themes. The bundled "
                             "themes show which keys you can set.",
                             box),
               theme_);
  nav_->RegisterRow(form, theme_, "theme appearance dark light");

  drag_select_ = new QCheckBox(box);
  drag_select_->setChecked(true);
  form->addRow("Drag to Select Games", drag_select_);
  nav_->RegisterRow(form, drag_select_, "drag to select rubber band multiple");

  nav_->AddSubheading(form, "Layout");

  // A 2-column grid that hugs its own content, not five rows the form's
  // AllNonFixedFieldsGrow policy stretches edge to edge: a pixel count next
  // to its label, not a text-input-width box around a two-digit number.
  auto* shape_grid_widget = new QWidget(box);
  shape_grid_widget->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
  auto* shape_grid = new QGridLayout(shape_grid_widget);
  shape_grid->setContentsMargins(0, 0, 0, 0);
  shape_grid->setHorizontalSpacing(20);
  shape_grid->setVerticalSpacing(8);
  shape_grid->addWidget(
      MakeShapeControl(tile_spacing_, "Tile Gap", 40,
                       "Empty space around each tile. The zoom slider in the top bar changes the "
                       "tile size."),
      0, 0);
  shape_grid->addWidget(
      MakeShapeControl(grid_margin_, "Grid Padding", 60,
                       "Space between the grid and the window edges and side panels."),
      0, 1);
  shape_grid->addWidget(
      MakeShapeControl(tile_radius_, "Cover Rounding", 40, "Corner radius of game covers. 0 is square."),
      1, 0);
  shape_grid->addWidget(
      MakeShapeControl(panel_radius_, "Panel Rounding", 24, "Corner radius of panels and toasts."),
      1, 1);
  shape_grid->addWidget(
      MakeShapeControl(control_radius_, "Control Rounding", 20,
                       "Corner radius of buttons, inputs and dropdowns."),
      2, 0);
  form->addRow(shape_grid_widget);
  nav_->RegisterRow(form, shape_grid_widget,
                    "tile gap grid padding cover rounding panel rounding control rounding "
                    "corner radius spacing layout");
  RefreshShapeDefaults();
  connect(mira_gui::theme::Notifier::Instance(), &mira_gui::theme::Notifier::Changed, this,
          &SettingsPanel::RefreshShapeDefaults);

  auto* note = new QLabel("These settings are saved in frontend.toml and only affect this app.", box);
  note->setWordWrap(true);
  note->setProperty("role", "muted");
  form->addRow(note);

  LoadFrontendPrefs();
}

QWidget* SettingsPanel::MakeShapeControl(ShapeField& field, const QString& label, int maximum,
                                         const QString& tip) {
  auto* row = new QWidget(this);
  auto* row_layout = new QHBoxLayout(row);
  row_layout->setContentsMargins(0, 0, 0, 0);
  row_layout->setSpacing(8);

  row_layout->addWidget(LabelWithHelp(label, tip, row), /*stretch=*/1);

  field.spin = new QSpinBox(row);
  // -1 rather than 0: 0 is a real radius, and "no rounding at all" has to
  // stay distinguishable from "leave it to the theme".
  field.spin->setRange(-1, maximum);
  field.spin->setValue(-1);
  field.spin->setSuffix(" px");
  // Wide enough for the special-value text (e.g. "60 px (theme default)"),
  // not just a couple of digits.
  field.spin->setFixedWidth(190);
  row_layout->addWidget(field.spin);

  return row;
}

void SettingsPanel::RefreshShapeDefaults() {
  const mira_gui::theme::Tokens& defaults = mira_gui::theme::ThemeDefaults();
  const auto set = [](ShapeField& field, int value) {
    field.spin->setSpecialValueText(QString("%1 px (theme default)").arg(value));
  };
  set(tile_spacing_, defaults.tile_spacing);
  set(grid_margin_, defaults.grid_margin);
  set(tile_radius_, defaults.radius_tile);
  set(panel_radius_, defaults.radius_panel);
  set(control_radius_, defaults.radius_control);
}

void SettingsPanel::BuildShortcutsGroup() {
  // Synchronous, unlike every other pref below: the owning window already
  // ran BuildShortcuts() and loaded overrides before this screen could open,
  // so ui/KeyBindings' registry already holds current state.
  auto* form = nav_->AddCategory("Shortcuts");
  for (const mira_gui::keybindings::Binding& binding : mira_gui::keybindings::All()) {
    const QKeySequence current =
        mira_gui::keybindings::Override(binding.id).value_or(binding.default_keys);

    auto* row_widget = new QWidget(this);
    auto* row_layout = new QHBoxLayout(row_widget);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(8);

    auto* edit = new QKeySequenceEdit(current, row_widget);
    edit->setMaximumSequenceLength(1);
    row_layout->addWidget(edit, /*stretch=*/1);

    auto* reset_button = new QPushButton("Reset", row_widget);
    reset_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    QString tooltip =
        QString("Reset to default: %1")
            .arg(binding.default_keys.isEmpty()
                     ? QString("no shortcut")
                     : binding.default_keys.toString(QKeySequence::NativeText));
    if (!binding.extra_aliases.isEmpty()) {
      QStringList alias_text;
      for (const QKeySequence& alias : binding.extra_aliases) {
        alias_text << alias.toString(QKeySequence::NativeText);
      }
      tooltip += QString(". %1 always works too and cannot be changed.").arg(alias_text.join(", "));
    }
    reset_button->setToolTip(tooltip);
    row_widget->setToolTip(tooltip);
    row_layout->addWidget(reset_button);

    shortcuts_.push_back(ShortcutField{binding.id, edit, current, reset_button});
    const size_t index = shortcuts_.size() - 1;
    connect(reset_button, &QPushButton::clicked, this,
            [this, index, default_keys = binding.default_keys] {
              shortcuts_[index].edit->setKeySequence(default_keys);
            });

    form->addRow(binding.label, row_widget);
    nav_->RegisterRow(form, row_widget, QString("%1 shortcut keyboard").arg(binding.label));
  }
}

void SettingsPanel::LoadFrontendPrefs() {
  theme_original_ = mira_gui::theme::CurrentName();
  const int theme_index = theme_->findData(theme_original_);
  if (theme_index >= 0) theme_->setCurrentIndex(theme_index);

  mira_gui::MiradClient::GetFrontendPrefsAsync(this, [this](mira_gui::FrontendPrefsResult result) {
    if (!result.ok) return;  // the defaults are already shown
    if (result.prefs.scan_on_startup) {
      scan_on_startup_original_ = *result.prefs.scan_on_startup;
      scan_on_startup_->setChecked(scan_on_startup_original_);
    }
    if (result.prefs.theme) {
      theme_original_ = QString::fromStdString(*result.prefs.theme);
      const int index = theme_->findData(theme_original_);
      if (index >= 0) theme_->setCurrentIndex(index);
    }
    if (result.prefs.drag_select) {
      drag_select_original_ = *result.prefs.drag_select;
      drag_select_->setChecked(drag_select_original_);
    }
    recent_count_original_ = result.prefs.sidebar_recent_count.value_or(recent_count_original_);
    recent_count_->setValue(recent_count_original_);
    recent_count_original_ = recent_count_->value();  // after the clamp
    source_counts_original_ = result.prefs.sidebar_source_counts.value_or(true);
    source_counts_->setChecked(source_counts_original_);
    hidden_sources_original_.clear();
    for (const std::string& id : result.prefs.hidden_sources.value_or(std::vector<std::string>{})) {
      hidden_sources_original_.insert(QString::fromStdString(id));
    }
    for (const auto& [id, check] : source_checks_) check->setChecked(!hidden_sources_original_.contains(id));
    const auto shape = [](ShapeField& field, const std::optional<int>& pref) {
      field.spin->setValue(pref ? *pref : -1);
      field.original = field.spin->value();  // after the clamp
    };
    shape(tile_spacing_, result.prefs.tile_spacing);
    shape(grid_margin_, result.prefs.grid_margin);
    shape(tile_radius_, result.prefs.tile_radius);
    shape(panel_radius_, result.prefs.panel_radius);
    shape(control_radius_, result.prefs.control_radius);
  });
}

void SettingsPanel::Load() {
  mira_gui::MiradClient::GetConfigSchemaAsync(this, [this](mira_gui::ConfigSchemaResult schema) {
    if (!schema.ok) {
      emit LoadFailed(QString::fromStdString(schema.error));
      return;
    }

    for (mira_gui::ConfigSchemaEntry& entry : schema.entries) {
      if (entry.game_only) continue;
      Field field;
      field.entry = std::move(entry);
      fields_.push_back(std::move(field));
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
        field.SetText(it != config.values.end() ? it->second : field.entry.default_display);
        field.original = field.Text();
      }
      setEnabled(true);
      if (!pending_focus_key_.isEmpty()) FocusKey(std::exchange(pending_focus_key_, QString()));
    });
  });
}

void SettingsPanel::FocusKey(const QString& key) {
  const std::string wanted = key.toStdString();
  if (key == kSidebarKey) {
    nav_->RevealRow(recent_count_);
    return;
  }
  const auto it = std::ranges::find(fields_, wanted, [](const Field& f) { return f.entry.key; });
  if (it == fields_.end()) {
    // Schema not loaded yet, most likely. Try again once it is.
    pending_focus_key_ = key;
    return;
  }

  nav_->RevealRow(it->row_widget);

  QWidget* field_widget = it->Input();
  if (field_widget == nullptr) return;
  field_widget->setFocus(Qt::OtherFocusReason);
  if (auto* line = qobject_cast<QLineEdit*>(field_widget)) line->selectAll();
}

void SettingsPanel::BuildRows() {
  std::vector<std::string> categories;
  for (const Field& field : fields_) categories.push_back(field.entry.category);

  for (const auto& [category, rows] : GroupByCategory(categories)) {
    QFormLayout* form = nav_->AddCategory(category);
    category_forms_[category] = form;
    int group = fields_[rows.front()].entry.group;

    for (const size_t i : rows) {
      if (fields_[i].entry.group != group) {
        nav_->AddDivider(form);
        group = fields_[i].entry.group;
      }

      Field& field = fields_[i];
      field.owner_form = form;
      QWidget* row_widget = field.Build(this, [this, i] { ResetField(i); });

      form->addRow(field.BuildLabel(this), row_widget);
      nav_->RegisterRow(form, row_widget, field.SearchText());
    }

    if (category == "Launching") {
      gamemode_status_ = new QLabel("Checking…", this);
      mira_gui::theme::SetStyleProperty(gamemode_status_, "role", "muted");
      form->addRow("GameMode", gamemode_status_);
      nav_->RegisterRow(form, gamemode_status_, "gamemode feral daemon status");
      LoadGameModeStatus();
    }
  }
  rows_built_ = true;
  for (const SectionAction& action : section_actions_) AppendSectionAction(action);
}

void SettingsPanel::AddSectionAction(const QString& category, const QString& label, const QString& doc,
                                     const QString& button_text, std::function<void()> activated) {
  section_actions_.push_back({category, label, doc, button_text, std::move(activated)});
  if (rows_built_) AppendSectionAction(section_actions_.back());
}

void SettingsPanel::AppendSectionAction(const SectionAction& action) {
  QFormLayout* form = category_forms_.value(action.category);
  if (form == nullptr) return;
  if (!categories_with_actions_.contains(action.category)) {
    nav_->AddDivider(form);
    categories_with_actions_.insert(action.category);
  }
  auto* button = new QPushButton(action.button_text, this);
  button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  connect(button, &QPushButton::clicked, this, action.activated);
  form->addRow(LabelWithHelp(action.label, action.doc, this), button);
  nav_->RegisterRow(form, button, QString("%1 %2 %3").arg(action.label, action.category, action.doc));
}

void SettingsPanel::LoadGameModeStatus() {
  mira_gui::MiradClient::GetGameModeStatusAsync(this, [this](mira_gui::GameModeStatusResult result) {
    if (gamemode_status_ == nullptr) return;
    QString text;
    bool error = false;
    if (!result.ok) {
      text = "Could not check (mirad unreachable).";
      error = true;
    } else if (!result.installed) {
      text = "Not installed: gamemoded isn't on PATH.";
      error = true;
    } else if (!result.daemon_running) {
      text = "Installed, but the daemon isn't running right now.";
      error = true;
    } else {
      text = "Installed and running.";
    }
    gamemode_status_->setText(text);
    mira_gui::theme::SetStyleProperty(gamemode_status_, "role", error ? "error" : "muted");
  });
}

void SettingsPanel::PopulateRunnerCombos(const mira_gui::RunnersResult& result) {
  for (Field& field : fields_) {
    if (field.combo != nullptr && field.entry.is_runner_ref) FillRunnerCombo(field.combo, result);
  }
}

void SettingsPanel::ResetField(size_t index) {
  mira_gui::MiradClient::ResetConfigKeyAsync(
      this, fields_[index].entry.key, [this, index](mira_gui::PatchConfigResult result) {
        if (!result.ok) {
          emit SaveFinished(false, QString::fromStdString(result.error));
          return;
        }
        Field& field = fields_[index];
        field.SetText(field.entry.default_display);
        field.original = field.Text();
      });
}

void SettingsPanel::SetFooterActions(QWidget* actions) { nav_->AddFooterWidget(actions); }

bool SettingsPanel::IsDirty() const {
  if (scan_on_startup_->isChecked() != scan_on_startup_original_) return true;
  if (theme_->currentData().toString() != theme_original_) return true;
  if (drag_select_->isChecked() != drag_select_original_) return true;
  if (SidebarDirty()) return true;
  for (const ShapeField* field :
       {&tile_spacing_, &grid_margin_, &tile_radius_, &panel_radius_, &control_radius_}) {
    if (field->spin->value() != field->original) return true;
  }
  for (const ShortcutField& field : shortcuts_) {
    if (field.edit->keySequence() != field.original) return true;
  }
  for (const Field& field : fields_) {
    if (field.Text() != field.original) return true;
  }
  return false;
}

void SettingsPanel::DiscardChanges() {
  scan_on_startup_->setChecked(scan_on_startup_original_);
  const int theme_index = theme_->findData(theme_original_);
  if (theme_index >= 0) theme_->setCurrentIndex(theme_index);
  drag_select_->setChecked(drag_select_original_);
  recent_count_->setValue(recent_count_original_);
  source_counts_->setChecked(source_counts_original_);
  for (const auto& [id, check] : source_checks_) check->setChecked(!hidden_sources_original_.contains(id));
  for (ShapeField* field :
       {&tile_spacing_, &grid_margin_, &tile_radius_, &panel_radius_, &control_radius_}) {
    field->spin->setValue(field->original);
  }
  for (ShortcutField& field : shortcuts_) field.edit->setKeySequence(field.original);
  for (Field& field : fields_) field.SetText(field.original);
}

void SettingsPanel::Save() {
  const QString theme_name = theme_->currentData().toString();
  bool shapes_changed = false;
  for (const ShapeField* field :
       {&tile_spacing_, &grid_margin_, &tile_radius_, &panel_radius_, &control_radius_}) {
    if (field->spin->value() != field->original) shapes_changed = true;
  }
  bool shortcuts_changed = false;
  for (const ShortcutField& field : shortcuts_) {
    if (field.edit->keySequence() != field.original) shortcuts_changed = true;
  }
  if (scan_on_startup_->isChecked() != scan_on_startup_original_ ||
      theme_name != theme_original_ || shapes_changed || shortcuts_changed ||
      drag_select_->isChecked() != drag_select_original_ || SidebarDirty()) {
    mira_gui::FrontendPrefs prefs;
    prefs.sidebar_recent_count = recent_count_->value();
    prefs.sidebar_source_counts = source_counts_->isChecked();
    const QSet<QString> hidden = CurrentHiddenSources();
    std::vector<std::string> hidden_ids;
    for (const QString& id : hidden) hidden_ids.push_back(id.toStdString());
    std::ranges::sort(hidden_ids);
    prefs.hidden_sources = std::move(hidden_ids);
    recent_count_original_ = *prefs.sidebar_recent_count;
    source_counts_original_ = *prefs.sidebar_source_counts;
    hidden_sources_original_ = hidden;
    prefs.scan_on_startup = scan_on_startup_->isChecked();
    prefs.theme = theme_name.toStdString();
    prefs.drag_select = drag_select_->isChecked();
    // Always written, including the -1 that means "theme default": the key
    // has to be able to go back to unset, and a merge-patch cannot drop one.
    prefs.tile_spacing = tile_spacing_.spin->value();
    prefs.grid_margin = grid_margin_.spin->value();
    prefs.tile_radius = tile_radius_.spin->value();
    prefs.panel_radius = panel_radius_.spin->value();
    prefs.control_radius = control_radius_.spin->value();
    for (ShapeField* field :
         {&tile_spacing_, &grid_margin_, &tile_radius_, &panel_radius_, &control_radius_}) {
      field->original = field->spin->value();
    }
    if (shapes_changed) {
      mira_gui::theme::Overrides overrides;
      const auto shape = [](const ShapeField& field) -> std::optional<int> {
        if (field.spin->value() < 0) return std::nullopt;
        return field.spin->value();
      };
      overrides.tile_spacing = shape(tile_spacing_);
      overrides.grid_margin = shape(grid_margin_);
      overrides.radius_tile = shape(tile_radius_);
      overrides.radius_panel = shape(panel_radius_);
      overrides.radius_control = shape(control_radius_);
      mira_gui::theme::SetOverrides(overrides);
    }
    if (shortcuts_changed) {
      const QList<mira_gui::keybindings::Binding> bindings = mira_gui::keybindings::All();
      for (ShortcutField& field : shortcuts_) {
        const QKeySequence current = field.edit->keySequence();
        if (current == field.original) continue;
        const auto binding = std::ranges::find(bindings, field.id, &mira_gui::keybindings::Binding::id);
        if (binding != bindings.end() && current == binding->default_keys) {
          mira_gui::keybindings::ResetOverride(field.id);
        } else {
          mira_gui::keybindings::SetOverride(field.id, current);
        }
        field.original = current;
      }
      prefs.shortcut_overrides = mira_gui::keybindings::Current();
    }
    scan_on_startup_original_ = *prefs.scan_on_startup;
    drag_select_original_ = *prefs.drag_select;
    if (theme_name != theme_original_) {
      theme_original_ = theme_name;
      mira_gui::theme::Apply(theme_name);
    }
    mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
  }

  std::vector<mira_gui::ConfigEdit> edits;
  for (const Field& field : fields_) {
    const std::string current = field.Text();
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
    for (Field& field : fields_) field.original = field.Text();
    emit SaveFinished(true, QString());
  });
}

}  // namespace mira_gui
