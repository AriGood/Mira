#pragma once

#include <QString>
#include <QStringList>

// Display-only ordering for SettingsDialog's category groups. The
// categories/secrets/runner-refs themselves come from the config schema.
namespace mira_gui::settings {

// Categories appear in this order when present; anything else (a future,
// unmapped prefix) is appended alphabetically after.
const QStringList& CategoryOrder();

}  // namespace mira_gui::settings
