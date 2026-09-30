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
// use, then every setting under its own keys. Like the settings screen,
// edits apply on Save; Reset and "Switch them too" are actions and apply at once.
class SourceSettingsCard : public QFrame {
  Q_OBJECT

public:
  SourceSettingsCard(const SourceInfo& source, QWidget* parent = nullptr);

  // Reloads everything from mirad, e.g. after the launcher was installed.
  void Refresh();

  bool IsDirty() const;
  // Emits SaveFinished once mirad has answered.
  void Save();
  void Discard();

signals:
  // "All settings…": the full settings screen at `focus_key`.
  void OpenSettingsRequested(QString focus_key);
  void SaveFinished(bool ok);

private:
  bool HasRunner() const;
  bool RunnerDirty() const;
  void BuildRunnerRow(QFormLayout* form);
  void LoadRunner();
  void ShowRunner(const SourceRunnerResult& runner);
  void SelectRunner(const QString& runner_ref);
  void LoadSettings();
  void ResetSetting(size_t index);
  void UpdateButtons();
  void ShowStatus(const QString& text, bool error);

  SourceInfo source_;
  std::string id_;

  QComboBox* runner_ = nullptr;
  QLabel* runner_note_ = nullptr;
  QPushButton* runner_apply_ = nullptr;
  bool runner_can_apply_ = false;  // some games use another runner
  QString runner_ref_;  // what mirad last confirmed

  QFormLayout* form_ = nullptr;
  std::vector<SettingEditor> settings_;
  QLabel* status_ = nullptr;
  QPushButton* discard_ = nullptr;
  QPushButton* save_ = nullptr;
  bool saving_ = false;
};

}  // namespace mira_gui
