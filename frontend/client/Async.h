#pragma once

#include <QCoreApplication>
#include <QObject>
#include <QPointer>

#include <functional>
#include <thread>
#include <utility>

// How every MiradClient call gets off the UI thread and back onto it.
namespace mira_gui::async {

// Hands `fn` to the main thread, dropping it if the object `guard` watches
// has been destroyed in the meantime.
//
// The delivery target is qApp, deliberately, and not the context object
// itself. QMetaObject::invokeMethod dereferences its context argument on the
// *calling* thread, so handing it a QObject* the main thread may already have
// deleted is a use-after-free before the queued call is ever posted.
template <typename Fn>
void Deliver(const QPointer<QObject>& guard, Fn fn) {
  QObject* app = QCoreApplication::instance();
  if (app == nullptr) return;
  QMetaObject::invokeMethod(
      app,
      [guard, fn = std::move(fn)]() mutable {
        if (guard.isNull()) return;
        fn();
      },
      Qt::QueuedConnection);
}

// Runs `work` on a throwaway thread and delivers its return value to
// `callback` on the main thread, subject to Deliver's liveness rule above.
// `Result` is deduced from the callback, so a caller writes only the request
// it actually wants to make.
template <typename Result, typename Work>
void Run(QObject* context, Work work, std::function<void(Result)> callback) {
  QPointer<QObject> guard(context);
  std::thread([guard, work = std::move(work), callback = std::move(callback)]() mutable {
    Result result = work();
    Deliver(guard, [callback, result = std::move(result)]() mutable { callback(std::move(result)); });
  }).detach();
}

}  // namespace mira_gui::async
