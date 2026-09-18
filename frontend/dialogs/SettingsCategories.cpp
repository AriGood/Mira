#include "SettingsCategories.h"

namespace mira_gui::settings {

const QStringList& CategoryOrder() {
  static const QStringList order = {"Library",  "Runners",  "Launching",       "Detection",
                                    "Scanning", "Metadata", "Desktop Entries", "Advanced",
                                    "General"};
  return order;
}
}  // namespace mira_gui::settings
