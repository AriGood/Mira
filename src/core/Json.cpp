#include "core/Json.h"

namespace mira::core {

nlohmann::json ParseJsonTail(const std::string& text) {
  size_t line_end = text.size();
  while (line_end > 0) {
    size_t line_start = text.rfind('\n', line_end - 1);
    line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
    const std::string_view line(text.data() + line_start, line_end - line_start);
    const size_t first = line.find_first_not_of(" \t\r");
    if (first != std::string_view::npos && (line[first] == '{' || line[first] == '[')) {
      const nlohmann::json parsed = nlohmann::json::parse(line.substr(first), nullptr, false);
      if (!parsed.is_discarded()) return parsed;
    }
    if (line_start == 0) break;
    line_end = line_start - 1;
  }
  return nlohmann::json::parse("", nullptr, false);
}

}  // namespace mira::core
