#include "api/Jobs.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>

#include "core/Log.h"

namespace mira::api {
namespace {

using nlohmann::json;

// Finished jobs kept for GET /v1/jobs/{id}, oldest dropped first.
constexpr std::size_t kKeptJobs = 100;

json ErrorJson(const Error& error) {
  json out = {{"code", error.code}, {"message", error.message}};
  if (!error.hint.empty()) out["hint"] = error.hint;
  if (!error.fix.kind.empty()) {
    out["fix"] = {{"kind", error.fix.kind}, {"target", error.fix.target}};
    if (!error.fix.step.empty()) out["fix"]["step"] = error.fix.step;
  }
  return out;
}

}  // namespace

bool JobRegistry::IsValidId(const std::string& id) {
  return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_';
  });
}

void JobRegistry::Progress::Report(int done, int total, const std::string& message) {
  registry_.Update(id_, [&](json& job) { job["progress"] = {{"done", done}, {"total", total}, {"message", message}}; });
  registry_.events_.Publish("job.progress", {{"id", id_}, {"done", done}, {"total", total}, {"message", message}});
}

std::string JobRegistry::Start(const std::string& kind, const std::string& target, const std::string& label,
                               Work work, const std::string& requested) {
  std::string id;
  {
    std::lock_guard lock(mutex_);
    id = IsValidId(requested) ? requested
                              : std::format("{}-{}-{}", kind,
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count(),
                                            next_++);
    jobs_.push_back({{"id", id}, {"kind", kind}, {"target", target}, {"label", label}, {"state", "running"}});
    while (jobs_.size() > kKeptJobs) jobs_.pop_front();
  }
  const json identity = {{"id", id}, {"kind", kind}, {"target", target}};
  json started = identity;
  started["label"] = label;
  events_.Publish("job.started", started);

  queue_.Run([this, id, identity, work = std::move(work)] {
    Progress progress(*this, id);
    Result<json> result = work(progress);
    json event = identity;
    if (result) {
      event["result"] = *result;
      Update(id, [&](json& job) {
        job["state"] = "finished";
        job["result"] = *result;
      });
      events_.Publish("job.finished", std::move(event));
    } else {
      log::Warn("{} {} failed: {}", identity.value("kind", ""), identity.value("target", ""), result.error().message);
      event["error"] = ErrorJson(result.error());
      Update(id, [&](json& job) {
        job["state"] = "failed";
        job["error"] = ErrorJson(result.error());
      });
      events_.Publish("job.failed", std::move(event));
    }
  });
  return id;
}

std::optional<json> JobRegistry::Find(const std::string& id) const {
  std::lock_guard lock(mutex_);
  const auto found = std::ranges::find_if(jobs_, [&](const json& job) { return job.value("id", "") == id; });
  if (found == jobs_.end()) return std::nullopt;
  return *found;
}

void JobRegistry::Update(const std::string& id, const std::function<void(json&)>& change) {
  std::lock_guard lock(mutex_);
  const auto found = std::ranges::find_if(jobs_, [&](const json& job) { return job.value("id", "") == id; });
  if (found != jobs_.end()) change(*found);
}

}  // namespace mira::api
