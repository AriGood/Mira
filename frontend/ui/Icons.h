#pragma once

#include <QIcon>

namespace mira_gui::icons {

// The glyphs the top bar draws. Deliberately not QStyle::standardIcon: those
// are the platform style's, which under Fusion are the dated 3D-ish titlebar
// buttons, and they take their color from the platform rather than from the
// theme.
enum class Glyph {
  Menu,
  Settings,
  Minimize,
  Maximize,
  Restore,
  Close,
};

// Drawn on demand in the current theme's text color, at the handful of sizes
// Qt might ask for. Regenerate after a theme change — the color is baked in
// (see LibraryWindow's theme::Notifier connection).
QIcon For(Glyph glyph);

}  // namespace mira_gui::icons
