#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.h"

namespace mira::steam {

// A node in Valve's KeyValues/VDF text format — either a leaf string or an
// object of child nodes, exactly like the format itself has no separate
// array type: everything is nested objects and strings. Used for
// libraryfolders.vdf, appmanifest_*.acf, and localconfig.vdf, all the same
// syntax with different shapes.
struct VdfValue {
  std::optional<std::string> scalar;
  std::map<std::string, VdfValue> children;

  bool IsObject() const { return !scalar.has_value(); }

  // Walks a dotted/segmented path of child keys. Case-insensitive, since
  // real Steam files are inconsistent about it (`"apps"` vs some tools'
  // `"Apps"`). Returns nullptr anywhere the path doesn't match.
  const VdfValue* Get(std::initializer_list<std::string_view> path) const;
};

Result<VdfValue> ParseVdf(std::string_view text);

}  // namespace mira::steam
