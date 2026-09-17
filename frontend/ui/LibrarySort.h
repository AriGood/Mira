#pragma once

#include <string>
#include <vector>

#include "../client/Types.h"

namespace mira_gui {

// How the library grid orders its tiles.
//
// A free function over the summaries rather than a method on the window,
// because the ordering is a rule about games, not about widgets — and a
// rule about games is worth testing without an event loop.
//
// `key` is one of "name", "last_played", "playtime", "status"; anything
// else falls back to "name", so a hand-edited frontend.toml can't produce
// an unordered grid.
//
// The sort is stable, and every key breaks ties by name, so two games that
// have never been played keep a predictable order instead of shuffling on
// each refresh.
void SortGames(std::vector<GameSummary>& games, const std::string& key, bool descending);

// The keys SortGames understands, in the order a picker should offer them,
// paired with the label to show.
struct SortOption {
  const char* key;
  const char* label;
};
const std::vector<SortOption>& SortOptions();

}  // namespace mira_gui
