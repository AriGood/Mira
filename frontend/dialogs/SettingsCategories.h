#pragma once

#include <QString>
#include <QStringList>

#include <string>

// How SettingsDialog decides where a schema key belongs and how it should be
// edited.
//
// Pulled out of the dialog because it is the only part of that screen that
// is a policy rather than a widget: it is a list of judgements about
// src/config/Schema.cpp's keys, and it is what has to be revisited when the
// backend grows a setting — not the layout code around it.
namespace mira_gui::settings {

// True for the one schema key that should get a runner picker
// (GET /v1/runners) rather than a free-text box. The schema has no way to
// mark a string as "this is a runner reference", so it is named here.
bool IsRunnerKey(const std::string& key);

// Which group a dotted schema key is shown under. Everything currently in
// src/config/Schema.cpp is named explicitly; a key added later without a
// matching entry still lands somewhere sensible via a dotted-prefix guess,
// and never disappears.
QString CategoryFor(const std::string& key);

// Categories appear in this order when present; anything else (a future,
// unmapped prefix) is appended alphabetically after.
const QStringList& CategoryOrder();

}  // namespace mira_gui::settings
