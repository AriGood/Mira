#pragma once

#include <QColor>
#include <QString>

#include <vector>

namespace mira_gui {

struct SourceInfo {
  enum class Kind {
    Store,     // a helper tool, an account, and what the account owns
    Launcher,  // a Windows launcher installed into its own prefix
    Local,     // reads what another program already installed
  };
  QString id;    // mirad's own name: "steam", "epic", ...; also the games' `source`
  QString name;  // shown in the sidebar and as the page title
  Kind kind = Kind::Local;
  QColor color;  // the source's icon tile
};

const std::vector<SourceInfo>& AllSources();

// nullptr for a source id Mira doesn't list (a scanned or manual game).
const SourceInfo* FindSourceInfo(const QString& id);

}  // namespace mira_gui
