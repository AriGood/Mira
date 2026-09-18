#pragma once

#include <QString>
#include <QStringList>

// Display-only ordering for SettingsDialog's category groups (see
// SettingsDialog.h). The categories themselves, and which keys are secrets
// or runner references, now come straight from GET /v1/config/schema
// (ConfigSchemaEntry::category/is_secret/is_runner_ref) — this file used to
// carry hardcoded copies of all three because the schema had no way to say
// them itself.
namespace mira_gui::settings {

// Categories appear in this order when present; anything else (a future,
// unmapped prefix) is appended alphabetically after.
const QStringList& CategoryOrder();

}  // namespace mira_gui::settings
