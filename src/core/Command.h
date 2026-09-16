#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mira {

// The single currency for executing anything. A bare exec, a wine invocation
// and a containerised Proton entry point differ only in argv, so every runner
// and every wrapper produces one of these and core needs to know nothing more.
struct Command {
  std::vector<std::string> argv;
  std::map<std::string, std::string> env;  // overlay on the daemon's environment
  std::filesystem::path cwd;
};

}  // namespace mira
