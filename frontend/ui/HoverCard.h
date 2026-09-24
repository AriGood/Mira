#pragma once

#include <QRect>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <string>

#include "../client/Types.h"

class QLabel;
class QListWidgetItem;

namespace mira_gui {

// Look and placement shared by HoverCard and the app's tooltips (ToolTip.h),
// so every hover popup has the same padding, corners and gap.
namespace card {
inline constexpr int kPaddingX = 12;
inline constexpr int kPaddingY = 10;
inline constexpr int kGap = 8;  // between the popup and what it describes
// A card popping up on everything the cursor merely crosses would be worse
// than none.
inline constexpr int kDwellMs = 280;
void Paint(QWidget* widget);
// Global top-left for a popup of `size` next to `anchor` (global). `beside`:
// right of it, else left. Otherwise below it, else above. Kept on screen.
QPoint Place(const QRect& anchor, QSize size, bool beside);
}  // namespace card

// Calls on_hover with an item once the cursor has rested on it, and with
// nullptr at once when it moves to another item or off the grid.
class HoverDwell {
public:
  explicit HoverDwell(std::function<void(QListWidgetItem*)> on_hover);
  void Track(QListWidgetItem* hovered);
  // Before the items are deleted.
  void Forget();

private:
  std::function<void(QListWidgetItem*)> on_hover_;
  QTimer timer_;
  QListWidgetItem* last_ = nullptr;
};

// A floating, non-modal preview shown after a short dwell over a game: name,
// status, ProtonDB tier, platform, last played, playtime, developer/genres.
// Install path, description and reviews are left out: this is a glance; the
// per-game edit page and GameDetailPageDialog have them.
class HoverCard : public QWidget {
  Q_OBJECT

public:
  explicit HoverCard(QWidget* parent = nullptr);

  // Fills in what's already known (no round trip), then fetches the rest
  // (developer/genres/ProtonDB tier) with one GET /v1/games/{id}/metadata.
  // Safe to call again for a different game while still visible -- the
  // fetch result is dropped if a newer ShowGame has since moved on.
  // `hint` is a muted last line, e.g. what a click does.
  void ShowGame(const GameSummary& game, bool running, const QString& hint = QString());
  // A store title that isn't in the library yet.
  void ShowTitle(const QString& title, const QString& status, const QString& detail);
  // Shows the card beside `anchor` (global), and keeps it there as it grows.
  void PopUpBeside(const QRect& anchor);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void ShowMetadata(const GameMetadata& metadata);
  void Reset();
  void Reposition();

  std::string game_id_;
  QRect anchor_;
  QLabel* name_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* protondb_ = nullptr;
  QLabel* platform_line_ = nullptr;
  QLabel* played_line_ = nullptr;
  QLabel* developer_line_ = nullptr;
  QLabel* error_line_ = nullptr;
  QLabel* hint_line_ = nullptr;
};

}  // namespace mira_gui
