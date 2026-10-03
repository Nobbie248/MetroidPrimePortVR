// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's launcher window (DolphinQt/MainWindow.cpp ConnectStack) for the
// native port: the tabs, the footer, and the game as a child process. The
// settings are edited in memory, written to port_settings.ini by Save Settings
// or Play, and locked while the game runs, since the game rewrites that file
// itself when it exits.

#pragma once

#include "settings_model.h"
#include "tab_context.h"

#include <QMainWindow>
#include <QProcess>
#include <QSettings>

#include <string>
#include <string_view>
#include <vector>

class QFileSystemWatcher;
class QLabel;
class QPushButton;
class QTabWidget;
class QTimer;
class QVBoxLayout;

namespace PrimedGunLauncher {

class LauncherWindow final : public QMainWindow {
public:
  LauncherWindow();

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void BuildSetupTab(QTabWidget* tabs);
  void BuildFooter(QVBoxLayout* layout);

  void LoadSettings();
  bool SaveSettings();
  void SaveKeyNow(std::string_view key);
  void WatchSettingsFile();
  void OnSettingsFileChanged();
  void UpdateFooter();

  QString SelectedDisc() const;
  void SetSelectedDisc(const QString& path);
  void UpdateSelectedGame();
  void SelectGame();
  void ShowGameOptions();
  void ShowSetupNotes();
  void TransferOldSave();

  void Play();
  void Stop();
  void OnGameFinished(int exitCode, QProcess::ExitStatus status);
  void SetRunning(bool running);

  LauncherPaths m_paths;
  SettingsModel m_model;
  TabContext m_ctx;
  QSettings m_ini;

  QTabWidget* m_tabs = nullptr;
  std::vector<QWidget*> m_settingsPages;
  QLabel* m_selectedGame = nullptr;
  QLabel* m_discWarning = nullptr;
  QLabel* m_runStatus = nullptr;
  QPushButton* m_selectButton = nullptr;
  QPushButton* m_playButton = nullptr;
  QPushButton* m_stopButton = nullptr;
  QPushButton* m_optionsButton = nullptr;
  QPushButton* m_transferButton = nullptr;
  QPushButton* m_resetAllButton = nullptr;
  QPushButton* m_saveButton = nullptr;
  QLabel* m_footerStatus = nullptr;

  QProcess* m_game = nullptr;
  QTimer* m_forceStopTimer = nullptr;
  bool m_stopRequested = false;
  bool m_forceStopOffered = false;
  bool m_closeAfterStop = false;

  QFileSystemWatcher* m_watcher = nullptr;
  std::string m_lastWritten; // what the launcher itself last wrote, to ignore its own change
  bool m_changedOnDisk = false;
};

} // namespace PrimedGunLauncher
