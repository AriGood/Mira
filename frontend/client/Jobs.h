#pragma once

#include <QObject>

#include <json.hpp>

#include <functional>
#include <string>

#include "ApiError.h"

// Waiting on mirad's jobs (docs/api.md#jobs): a long request answers 202 at
// once and its outcome arrives later as job.finished or job.failed.
namespace mira_gui::jobs {

struct Outcome {
  bool ok = false;
  nlohmann::json result;  // the endpoint's documented reply, when ok
  ApiError error;
};

// A fresh id to start a job under (`?job=`), so the waiter is listening
// before the 202 lands.
std::string NewToken(const std::string& kind);

// Calls `done` once, on the main thread, when job `token` ends. Dropped if
// `context` is destroyed first. Survives a reconnect: pending jobs are
// looked up again, and one mirad no longer knows about fails.
void Await(QObject* context, const std::string& token, std::function<void(Outcome)> done);

// Stops waiting. False when the job already ended and `done` has run.
bool Forget(const std::string& token);

}  // namespace mira_gui::jobs
