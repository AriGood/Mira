#include "SettingsCategories.h"

namespace mira_gui::settings {

// Categories appear in this order when present; an unmapped one is
// appended alphabetically after.
const QStringList& CategoryOrder() {
  static const QStringList order = {"Library",  "Runners",  "Launching",       "Detection",
                                    "Scanning", "Metadata", "Desktop Entries", "Advanced",
                                    "General"};
  return order;
}
}  // namespace mira_gui::settings
