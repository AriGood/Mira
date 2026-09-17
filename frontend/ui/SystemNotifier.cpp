#include "SystemNotifier.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QStringList>
#include <QVariantMap>

namespace mira_gui::notify::system_notifier {
namespace {

constexpr char kService[] = "org.freedesktop.Notifications";
constexpr char kPath[] = "/org/freedesktop/Notifications";

// Matches packaging/mira.desktop's basename. The desktop-entry hint is what
// lets Plasma show Mira's own name and icon on the notification, and what
// puts Mira in the per-application notification settings — without it every
// message arrives from an unnamed application.
constexpr char kDesktopEntry[] = "mira";

// 0 low, 1 normal, 2 critical, per the freedesktop notification spec. A
// critical notification is not dismissed on a timeout by most shells, which
// is right for an error and wrong for everything else.
uchar UrgencyFor(Level level) {
  switch (level) {
    case Level::Error: return 2;
    case Level::Warning: return 1;
    case Level::Info:
    case Level::Success: break;
  }
  return 0;
}

int TimeoutFor(Level level) {
  switch (level) {
    case Level::Error: return 10000;
    case Level::Warning: return 8000;
    case Level::Info:
    case Level::Success: break;
  }
  return 5000;
}

QDBusInterface& Interface() {
  static QDBusInterface interface(kService, kPath, kService, QDBusConnection::sessionBus());
  return interface;
}

}  // namespace

bool Available() {
  static const bool available = Interface().isValid();
  return available;
}

bool Send(Level level, const QString& text) {
  if (!Available()) return false;

  QVariantMap hints;
  hints["urgency"] = QVariant::fromValue(UrgencyFor(level));
  hints["desktop-entry"] = QString(kDesktopEntry);

  // The summary is the app, the body is the message. Splitting them the
  // other way around would put a sentence in bold and leave the body empty,
  // which is how a notification ends up unreadable at a glance.
  const QDBusReply<uint> reply =
      Interface().call("Notify", QString("Mira"), 0U, QString(kDesktopEntry), QString("Mira"), text,
                       QStringList(), hints, TimeoutFor(level));
  return reply.isValid();
}

}  // namespace mira_gui::notify::system_notifier
