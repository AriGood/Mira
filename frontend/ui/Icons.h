#pragma once

#include <QIcon>

class QColor;

namespace mira_gui::icons {

// Every glyph the top bar and sidebar draw. Not QStyle::standardIcon --
// under Fusion those are dated 3D titlebar buttons in the platform's color,
// not the theme's.
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
  Store,
  Sliders,
  Dot,
};

// Drawn on demand in the current theme's text color, at the handful of sizes
// Qt might ask for. Regenerate after a theme change — the color is baked in
// (see LibraryWindow's theme::Notifier connection).
QIcon For(Glyph glyph);

// Same, in a caller-chosen color — a muted row or an on_accent icon,
// neither the plain text color the no-argument overload assumes.
QIcon For(Glyph glyph, const QColor& color);

}  // namespace mira_gui::icons
