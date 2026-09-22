#pragma once

#include <memory>
#include <string>
#include <vector>

#include "library/ILibrarySource.h"

namespace mira::library {

// Every source that implements ILibrarySource, in a fixed order (matches
// the order sources are checked historically: epic, steam, then whatever's
// added after). Built once, lazily -- each source is stateless (real state
// lives in the config/games/events references every call takes), so one
// process-lifetime instance is enough.
const std::vector<std::unique_ptr<ILibrarySource>>& AllSources();

// nullptr if no source is registered under that name.
ILibrarySource* FindSource(const std::string& name);

}  // namespace mira::library
