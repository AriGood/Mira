#pragma once

#include <QFrame>
#include <QString>

#include <vector>

#include "../ui/SettingEditor.h"
#include "SourcePage.h"

class QComboBox;
class QFormLayout;
class QLabel;
class QPushButton;

namespace mira_gui {

// A source page's settings, opened from its banner: the runner its games
// use, then every setting under its own keys. Changes apply as they're
// made, with no Save step.
class SourceSettingsCard : public QFrame {
  Q_OBJECT

public:
  SourceSettingsCard(const SourceInfo& source, QWidget* parent = nullptr);

  // Reloads everything from mirad, e.g. after the launcher was installed.
  void Refresh();

signals:
  // "All settings…": the full settings screen at `focus_key`.
  void OpenSettingsRequested(QString focus_key);

private:
  bool HasRunner() const;
  void BuildRunnerRow(QFormLayout* form);
  void LoadRunner();
  void ShowRunner(const SourceRunnerResult& runner);
  void SetRunner(const QString& runner_ref, bool apply_to_games);
  void LoadSettings();
  void Commit(size_t index);
  void ResetSetting(size_t index);
  void ShowStatus(const QString& text, bool error);

  SourceInfo source_;
  std::string id_;
  bool loading_ = false;  // SetText while loading must not commit

  QComboBox* runner_ = nullptr;
  QLabel* runner_note_ = nullptr;
  QPushButton* runner_apply_ = nullptr;
  QString runner_ref_;  // what mirad last confirmed

  QFormLayout* form_ = nullptr;
  std::vector<SettingEditor> settings_;
  QLabel* status_ = nullptr;
};

}  // namespace mira_gui
