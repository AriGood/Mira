#pragma once

#include <QIcon>

class QColor;

namespace mira_gui::icons {

// Every glyph the top bar and sidebar draw. Deliberately not
// QStyle::standardIcon: those are the platform style's, which under Fusion
// are the dated 3D-ish titlebar buttons, and they take their color from the
// platform rather than from the theme.
enum class Glyph {
  Menu,
  Settings,
  Minimize,
  Maximize,
  Restore,
  Close,
  Home,
  Table,
  Plus,
  Search,
  Filter,
  SortArrows,
  ChevronDown,
  Play,
  CheckCircle,
  Download,
  Clock,
  Warning,
  CircleX,
  Moon,
  EyeSlash,
  Wrench,
  Image,
  Grid,
  Trash,
  Refresh,
  Keyboard,
  Info,
};

// Drawn on demand in the current theme's text color, at the handful of sizes
// Qt might ask for. Regenerate after a theme change — the color is baked in
// (see LibraryWindow's theme::Notifier connection).
QIcon For(Glyph glyph);

// Same, in a caller-chosen color — a muted filter row next to the active
// one, or an icon painted in on_accent over an accent background, neither of
// which is the plain theme text color the no-argument overload assumes.
QIcon For(Glyph glyph, const QColor& color);

}  // namespace mira_gui::icons
