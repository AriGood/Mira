#pragma once

#include <QFrame>

#include "SidebarGames.h"

class QCheckBox;
class QSpinBox;

namespace mira_gui {

// The in-window card for how PINNED and RECENTLY PLAYED look. Every choice
// applies at once (the sidebar beside it shows the result), so there is no
// Save; the window stores each one as it's made.
class SidebarStyleCard : public QFrame {
  Q_OBJECT

public:
  struct Choices {
    sidebar::Style pinned = sidebar::Style::Covers;
    sidebar::Style recent = sidebar::Style::Covers;
    int recent_count = 0;
    bool recent_when = true;  // "Yesterday" beside each recently played game
  };
  // `pinned` and `recent` are what the sections would list (`recent` as for the
  // spin box's maximum), so each tile previews the user's own games.
  SidebarStyleCard(const Choices& choices, std::vector<GameSummary> pinned, std::vector<GameSummary> recent,
                   ArtworkStore* artwork, QWidget* parent = nullptr);

signals:
  void Changed(const mira_gui::SidebarStyleCard::Choices& choices);
  void CloseRequested();

private:
  // One tile per style, side by side; picking one sets `*target`.
  QWidget* MakeChoices(sidebar::Style* target, bool recent);
  // The games a section's preview in `style` draws, as its current choices would show them.
  std::vector<sidebar::PreviewGame> PreviewGames(bool recent, sidebar::Style style) const;

  Choices choices_;
  std::vector<GameSummary> pinned_;
  std::vector<GameSummary> recent_;
  ArtworkStore* artwork_;
  QCheckBox* when_ = nullptr;
  QSpinBox* recent_count_ = nullptr;
};

}  // namespace mira_gui
