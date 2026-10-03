// SPDX-License-Identifier: GPL-3.0-or-later

#include "tab_context.h"

#include "launcher_keys.h"
#include "settings_model.h"
#include "widgets.h"

#include "port_paths.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace PrimedGunLauncher {

namespace {

const char* kTagTooltip = "The game saves this setting but does not use it yet.";

QString FromUtf8Folder(const std::string& folder) { return QString::fromUtf8(folder); }

} // namespace

std::filesystem::path ToPath(const QString& path) {
  return std::filesystem::path(path.toStdU16String());
}

QString FromPath(const std::filesystem::path& path) {
  return QString::fromStdU16String(path.u16string());
}

void OpenFolder(const QString& folder) {
  QDir().mkpath(folder);
  QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void OpenFile(const QString& path) {
  if (QFileInfo::exists(path)) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
  } else {
    OpenFolder(QFileInfo(path).absolutePath());
  }
}

QString NewestLog(const LauncherPaths& paths) {
  const QFileInfo own(paths.gameLog);
  const QFileInfo captured(paths.lastRunLog);
  const bool ownIsNewer =
      own.exists() && (!captured.exists() || own.lastModified() > captured.lastModified());
  return ownIsNewer ? paths.gameLog : paths.lastRunLog;
}

LauncherPaths ResolvePaths() {
  LauncherPaths paths;
  paths.exeFolder = QDir::cleanPath(QCoreApplication::applicationDirPath());
#if defined(_WIN32)
  paths.gameExe = paths.exeFolder + QStringLiteral("/metroid_prime_port.exe");
#else
  paths.gameExe = paths.exeFolder + QStringLiteral("/metroid_prime_port");
#endif
  // The game's own answer (platform/include/port_paths.h), so both always
  // agree on where port_settings.ini lives.
  QString user = QDir::cleanPath(FromUtf8Folder(PortPaths::UserFolder()));
  if (user.isEmpty()) {
    user = paths.exeFolder;
  }
  paths.userFolder = user;
  paths.settingsFile = user + QStringLiteral("/port_settings.ini");
  paths.launcherIni = user + QStringLiteral("/primedgun_launcher.ini");
  paths.cardFolder = QDir::cleanPath(FromUtf8Folder(PortPaths::CardFolder()) + QStringLiteral("/USA/Card A"));
  const QString textures = qEnvironmentVariable("MP_USER_TEXTURES");
  paths.userTextures = textures.isEmpty() ? user + QStringLiteral("/user_textures")
                                          : QDir::cleanPath(textures);
  paths.lastRunLog = user + QStringLiteral("/primedgun_last_run.log");
  paths.gameLog = user + QStringLiteral("/metroid_prime_port.log");
  paths.cannon.library = ToPath(user + QStringLiteral("/primedgun/cannon_textures"));
  paths.cannon.shippedLibrary = ToPath(paths.exeFolder + QStringLiteral("/primedgun/cannon_textures"));
  paths.cannon.userTextures = ToPath(paths.userTextures);
  return paths;
}

void TabContext::RefreshAll() const {
  for (const auto& refresh : m_refreshers) {
    refresh();
  }
}

void TabContext::Edited() const {
  if (onEdited) {
    onEdited();
  }
}

QVBoxLayout* TabContext::MakeScrollTab(QTabWidget* tabs, const QString& name) const {
  auto* page = new QWidget(tabs);
  auto* pageLayout = new QVBoxLayout(page);
  pageLayout->setContentsMargins(10, 8, 10, 8);
  pageLayout->setSpacing(8);
  auto* scroll = new QScrollArea(page);
  scroll->setWidgetResizable(true);
  auto* content = new QWidget(scroll);
  auto* contentLayout = new QVBoxLayout(content);
  contentLayout->setContentsMargins(12, 10, 12, 10);
  contentLayout->setSpacing(8);
  scroll->setWidget(content);
  pageLayout->addWidget(scroll);
  tabs->addTab(page, name);
  return contentLayout;
}

QLabel* TabContext::Section(const QString& text) const {
  auto* label = new QLabel(text);
  label->setObjectName(QStringLiteral("PrimedGunSection"));
  return label;
}

void TabContext::Separator(QVBoxLayout* layout) const {
  auto* line = new QFrame;
  line->setFrameShape(QFrame::HLine);
  line->setStyleSheet(QStringLiteral("color: #444955;"));
  layout->addWidget(line);
}

QLabel* TabContext::TagFor(std::string_view key) const {
  const KeyInfo* info = FindKey(key);
  if (info == nullptr || info->active) {
    return nullptr;
  }
  auto* tag = new QLabel(QObject::tr("not active yet"));
  tag->setObjectName(QStringLiteral("PrimedGunTag"));
  tag->setToolTip(QObject::tr(kTagTooltip));
  return tag;
}

QCheckBox* TabContext::Check(QVBoxLayout* layout, const QString& text, std::string_view key,
                             const QString& tooltip, const QString& note) {
  auto* check = new QCheckBox(text);
  if (!tooltip.isEmpty()) {
    check->setToolTip(tooltip);
  }
  QLabel* tag = TagFor(key);
  if (tag != nullptr || !note.isEmpty()) {
    auto* row = new QHBoxLayout;
    row->addWidget(check);
    if (!note.isEmpty()) {
      auto* noteLabel = new QLabel(note);
      noteLabel->setObjectName(QStringLiteral("PrimedGunMuted"));
      // Wraps rather than widening the page past the window.
      noteLabel->setWordWrap(true);
      row->addWidget(noteLabel, 1);
    }
    if (tag != nullptr) {
      row->addWidget(tag);
    }
    if (note.isEmpty()) {
      row->addStretch();
    }
    layout->addLayout(row);
  } else {
    layout->addWidget(check);
  }
  const std::string name(key);
  QObject::connect(check, &QCheckBox::toggled, check, [this, name](bool checked) {
    model.SetBool(name, checked);
    Edited();
  });
  AddRefresher([this, check, name] {
    const QSignalBlocker blocker(check);
    check->setChecked(model.Bool(name));
  });
  return check;
}

FloatRow* TabContext::Float(QVBoxLayout* layout, const QString& label, std::string_view key) {
  const KeyInfo* info = FindKey(key);
  const std::string name(key);
  return FloatWith(
      layout, label, info->uiMin, info->uiMax, info->uiStep,
      [this, name] { return model.Float(name); },
      [this, name](float v) { model.SetFloat(name, v); }, key);
}

FloatRow* TabContext::FloatWith(QVBoxLayout* layout, const QString& label, double min, double max,
                                double step, std::function<float()> get,
                                std::function<void(float)> set, std::string_view tagKey) {
  auto* row = new FloatRow(
      label, min, max, step,
      [this, set](float v) {
        set(v);
        Edited();
      },
      TagFor(tagKey));
  layout->addWidget(row);
  AddRefresher([row, get] { row->SetValue(get()); });
  return row;
}

void TabContext::RadioPair(QVBoxLayout* layout, const QString& first, const QString& second,
                           std::string_view key, bool firstValue) {
  auto* firstButton = new QRadioButton(first);
  auto* secondButton = new QRadioButton(second);
  auto* group = new QButtonGroup(firstButton);
  group->addButton(firstButton);
  group->addButton(secondButton);
  group->setExclusive(true);
  auto* row = new QHBoxLayout;
  row->addWidget(firstButton);
  row->addWidget(secondButton);
  if (QLabel* tag = TagFor(key)) {
    row->addWidget(tag);
  }
  row->addStretch();
  layout->addLayout(row);

  const std::string name(key);
  QObject::connect(firstButton, &QRadioButton::toggled, firstButton,
                   [this, name, firstValue](bool checked) {
                     if (checked) {
                       model.SetBool(name, firstValue);
                       Edited();
                     }
                   });
  QObject::connect(secondButton, &QRadioButton::toggled, secondButton,
                   [this, name, firstValue](bool checked) {
                     if (checked) {
                       model.SetBool(name, !firstValue);
                       Edited();
                     }
                   });
  AddRefresher([this, name, firstValue, firstButton, secondButton] {
    const QSignalBlocker firstBlocker(firstButton);
    const QSignalBlocker secondBlocker(secondButton);
    const bool isFirst = model.Bool(name) == firstValue;
    firstButton->setChecked(isFirst);
    secondButton->setChecked(!isFirst);
  });
}

QComboBox* TabContext::Combo(QVBoxLayout* layout, const QString& label, std::string_view key,
                             const std::vector<std::pair<QString, QString>>& items) {
  auto* row = new QHBoxLayout;
  auto* text = new QLabel(label);
  text->setMinimumWidth(170);
  text->setMaximumWidth(170);
  auto* combo = new QComboBox;
  for (const auto& [itemText, value] : items) {
    combo->addItem(itemText, value);
  }
  combo->installEventFilter(WheelBlocker::Instance());
  row->addWidget(text);
  row->addWidget(combo, 0);
  if (QLabel* tag = TagFor(key)) {
    row->addWidget(tag);
  }
  row->addStretch();
  layout->addLayout(row);

  const std::string name(key);
  QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), combo,
                   [this, name, combo](int index) {
                     if (index >= 0) {
                       model.Set(name, combo->itemData(index).toString().toStdString());
                       Edited();
                     }
                   });
  AddRefresher([this, name, combo] {
    const QSignalBlocker blocker(combo);
    const int index = combo->findData(QString::fromStdString(model.Value(name)));
    combo->setCurrentIndex(index >= 0 ? index : 0);
  });
  return combo;
}

} // namespace PrimedGunLauncher
