#pragma once

#include <string>

#include <json.hpp>

namespace mira::core {

// Scans `text` from the end for the last line that parses as complete
// JSON on its own, skipping over any subprocess log noise around it (a
// tool's own log lines never parse as JSON, however many stray
// '{'/'['/']'/'}' characters they contain; legendary's "[Core] ..." and
// "[cli] ..." lines fool a naive "find the first bracket" scan). Shared by every source that mixes
// its --json payload with its own stderr/stdout log noise (Legendary,
// gogdl, and likely butler too).
//
// Returns a discarded json (is_discarded() true) if nothing parses --
// never json()'s ordinary null, which a caller checking only
// is_discarded() would misread as "found nothing, but that's a valid
// result" rather than the parse failure it actually is.
nlohmann::json ParseJsonTail(const std::string& text);

}  // namespace mira::core
