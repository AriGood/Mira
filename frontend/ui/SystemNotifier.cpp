#include "SystemNotifier.h"

#include <QDBusConnection>
#include <QFileInfo>
#include <QDBusInterface>
#include <QDBusReply>
#include <QStandardPaths>
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

// The spec's expire_timeout: milliseconds, or 0 for "never expire, the
// user must dismiss it". Which is exactly what CurrentTimeoutSeconds() == 0
// means, so the two agree without translation.
int ExpireTimeoutMs() { return CurrentTimeoutSeconds() * 1000; }

QDBusInterface& Interface() {
  static QDBusInterface interface(kService, kPath, kService, QDBusConnection::sessionBus());
  return interface;
}

}  // namespace

bool DesktopEntryInstalled() {
  static const bool installed = [] {
    const QString file = QString(kDesktopEntry) + ".desktop";
    QStringList roots = {QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)};
    roots += QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString& root : roots) {
      if (QFileInfo::exists(root + "/applications/" + file)) return true;
    }
    return false;
  }();
  return installed;
}

bool Available() {
  static const bool available = Interface().isValid();
  return available;
}

bool Send(Level level, const QString& text) {
  if (!Available()) return false;

  QVariantMap hints;
  hints["urgency"] = QVariant::fromValue(UrgencyFor(level));
  // Only when the entry is actually installed. Pointing the shell at a
  // desktop file that isn't there buys nothing and, on a portal-managed
  // desktop, is the same claim that produces the app-id warning above.
  if (DesktopEntryInstalled()) hints["desktop-entry"] = QString(kDesktopEntry);

  // The summary is the app, the body is the message. Splitting them the
  // other way around would put a sentence in bold and leave the body empty,
  // which is how a notification ends up unreadable at a glance.
  const QDBusReply<uint> reply =
      Interface().call("Notify", QString("Mira"), 0U, QString(kDesktopEntry), QString("Mira"), text,
                       QStringList(), hints, ExpireTimeoutMs());
  return reply.isValid();
}

}  // namespace mira_gui::notify::system_notifier
