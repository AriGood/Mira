#pragma once

#include <expected>
#include <string>
#include <utility>

namespace mira {

// Errors cross every interface boundary as values, never as exceptions: a
// runner implementation that throws must not be able to take down the daemon.
struct Error {
  std::string code;     // stable, machine-readable, e.g. "runner_missing"
  std::string message;  // human-readable, shown in the GUI
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> Err(std::string code, std::string message) {
  return std::unexpected(Error{std::move(code), std::move(message)});
}

}  // namespace mira
