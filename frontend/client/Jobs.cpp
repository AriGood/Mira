#include "Jobs.h"

#include <QCoreApplication>
#include <QPointer>
#include <QUuid>

#include <map>
#include <utility>

#include "Async.h"
#include "EventHub.h"
#include "JsonMapping.h"
#include "Transport.h"

namespace mira_gui::jobs {
namespace {

using nlohmann::json;

struct Waiter {
  QPointer<QObject> context;
  std::function<void(Outcome)> done;
};

// Main thread only.
std::map<std::string, Waiter>& Pending() {
  static std::map<std::string, Waiter> pending;
  return pending;
}

void Resolve(const std::string& token, Outcome outcome) {
  const auto found = Pending().find(token);
  if (found == Pending().end()) return;
  Waiter waiter = std::move(found->second);
  Pending().erase(found);
  if (!waiter.context.isNull()) waiter.done(std::move(outcome));
}

// A job.finished / job.failed event, or GET /v1/jobs/{id}'s record once it has ended.
Outcome FromRecord(const json& record, bool failed) {
  Outcome outcome;
  outcome.ok = !failed;
  if (failed) {
    outcome.error = mapping::ToApiError(record.value("error", json::object()));
  } else {
    outcome.result = record.value("result", json::object());
  }
  return outcome;
}

// Events for jobs missed while disconnected may be gone, so ask for each one.
void Recheck() {
  for (const auto& [token, waiter] : Pending()) {
    async::Run(QCoreApplication::instance(), [token] { return transport::Get("/v1/jobs/" + token); },
               std::function<void(transport::Reply)>([token](transport::Reply reply) {
                 if (reply.status == 404) {
                   Outcome lost;
                   lost.error = ApiError("mirad restarted before this finished.");
                   Resolve(token, std::move(lost));
                   return;
                 }
                 if (!reply.ok) return;  // still unreachable: the next reconnect asks again
                 const std::string state = reply.body.value("state", std::string());
                 if (state == "finished" || state == "failed") Resolve(token, FromRecord(reply.body, state == "failed"));
               }));
  }
}

void Listen() {
  static bool listening = false;
  if (listening) return;
  listening = true;
  EventHub* hub = EventHub::Instance();
  QObject::connect(hub, &EventHub::Received, hub, [](const std::string& type, const std::string& data, bool) {
    if (type != "job.finished" && type != "job.failed") return;
    const json event = json::parse(data, nullptr, false);
    if (!event.is_object()) return;
    Resolve(event.value("id", std::string()), FromRecord(event, type == "job.failed"));
  });
  QObject::connect(hub, &EventHub::ConnectionChanged, hub, [](bool connected) {
    if (connected) Recheck();
  });
  hub->Start();
}

}  // namespace

std::string NewToken(const std::string& kind) {
  return kind + "-" + QUuid::createUuid().toString(QUuid::Id128).toStdString();
}

void Await(QObject* context, const std::string& token, std::function<void(Outcome)> done) {
  Listen();
  Pending()[token] = {context, std::move(done)};
}

bool Forget(const std::string& token) { return Pending().erase(token) > 0; }

}  // namespace mira_gui::jobs
