#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include <string>
#include <vector>

#include "../client/EventStream.h"
#include "../client/Types.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QVBoxLayout;

namespace mira_gui {

struct SourceInfo {
  QString id;    // mirad's own name: "steam", "epic", ...; also its `<id>.enabled` key
  QString name;  // shown in the sidebar and as the page title
};

// Every source the sidebar lists, in order.
const std::vector<SourceInfo>& AllSources();

// One store or launcher, as a full page in the library window: its helper
// tool, sign-in, importing what's already installed, and what the account
// owns. Rebuilt on every open, so it starts from mirad's current state.
class SourcePage : public QWidget {
  Q_OBJECT

public:
  SourcePage(const SourceInfo& source, QWidget* parent = nullptr);

signals:
  void BackRequested();
  // Games were imported or installed; the grid should relist.
  void LibraryChanged();
  void OpenSettingsRequested(QString focus_key);

private:
  bool IsStore() const;  // epic, gog, itch, humble: a helper tool and an account
  bool HasOwnedList() const;

  QWidget* BuildHelperSection();
  QWidget* BuildAccountSection();
  QWidget* BuildImportSection();
  QWidget* BuildOwnedSection();

  void RefreshStatus();
  void ApplyStatus(const StoreStatusResult& status);
  void SignIn();
  void Import();
  void RefreshOwned();
  void ShowOwned(const StoreLibraryResult& result);
  void ShowBundles(const HumbleLibraryResult& result);
  void FilterOwned();
  // The Install/Update/Download button for row `ref`, or nullptr.
  QPushButton* OwnedButton(const std::string& ref) const;
  void HandleEvent(const std::string& type, const std::string& data);

  SourceInfo source_;
  std::string id_;
  bool tool_installed_ = false;
  bool authenticated_ = false;

  QWidget* helper_section_ = nullptr;
  QLabel* helper_text_ = nullptr;
  QPushButton* setup_button_ = nullptr;
  QLabel* helper_error_ = nullptr;

  QWidget* account_section_ = nullptr;
  QLabel* account_text_ = nullptr;
  QWidget* sign_in_row_ = nullptr;
  QPushButton* open_login_ = nullptr;
  QLineEdit* credential_ = nullptr;
  QPushButton* sign_in_ = nullptr;
  QPushButton* sign_out_ = nullptr;
  QLabel* account_error_ = nullptr;
  std::string login_url_;

  QWidget* import_section_ = nullptr;
  QPushButton* import_button_ = nullptr;
  QLabel* import_result_ = nullptr;

  QWidget* owned_section_ = nullptr;
  QLabel* owned_note_ = nullptr;
  QLineEdit* owned_filter_ = nullptr;
  QTableWidget* owned_table_ = nullptr;
  QPushButton* owned_refresh_ = nullptr;
  // ref -> whether it's installed, for the rows owned_table_ holds.
  QHash<QString, bool> owned_installed_;

  EventStream event_stream_;
};

}  // namespace mira_gui
