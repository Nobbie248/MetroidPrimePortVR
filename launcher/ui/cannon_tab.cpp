// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's Cannon Textures tab (DolphinQt/MainWindow.cpp ConnectStack): pick
// one of PrimedGun's arm-cannon texture slots, or import a custom one, and
// apply it to the game's user texture pack (launcher/core/cannon_textures.h).

#include "tabs.h"

#include "cannon_textures.h"
#include "dds_preview.h"
#include "settings_model.h"
#include "tab_context.h"

#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <array>
#include <memory>

namespace PrimedGunLauncher {
namespace {

QImage LoadPreview(const QString& path) {
  if (path.isEmpty()) {
    return {};
  }
  QImage image(path);
  if (!image.isNull()) {
    return image;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  const QByteArray bytes = file.readAll();
  const RgbaImage decoded =
      DecodeDxt1Dds(reinterpret_cast<const uint8_t*>(bytes.constData()), static_cast<size_t>(bytes.size()));
  if (decoded.Empty()) {
    return {};
  }
  return QImage(decoded.pixels.data(), decoded.width, decoded.height, decoded.width * 4,
                QImage::Format_RGBA8888)
      .copy();
}

void SetPreview(QLabel* label, const QString& path) {
  label->setToolTip(path);
  const QImage image = LoadPreview(path);
  if (image.isNull()) {
    label->clear();
    label->setText(QObject::tr("No preview"));
    return;
  }
  label->setText(QString());
  label->setPixmap(QPixmap::fromImage(image).scaled(64, 64, Qt::KeepAspectRatio,
                                                    Qt::SmoothTransformation));
}

void SetPathLabel(QLabel* label, const QString& text) {
  label->setText(text);
  label->setToolTip(text);
}

QString SlotName(int slot) { return QString::fromStdString(Cannon::SlotName(slot)); }

struct CannonWidgets {
  QButtonGroup* slotGroup = nullptr;
  QLabel* status = nullptr;
  std::array<QLabel*, 3> paths{};
  std::array<QLabel*, 3> previews{};
  std::array<QPushButton*, 3> imports{};
};

void RefreshRows(const TabContext& ctx, const CannonWidgets& w) {
  const int slot = w.slotGroup->checkedId();
  for (int index = 0; index < 3; ++index) {
    w.imports[index]->setVisible(slot == Cannon::kCustomSlot);
    if (slot <= 0) {
      SetPathLabel(w.paths[index], QObject::tr("Default: no PrimedGun override"));
      SetPreview(w.previews[index], FromPath(Cannon::DefaultPreview(ctx.paths.cannon, index)));
      continue;
    }
    const QString path = FromPath(Cannon::ResolveSource(ctx.paths.cannon, slot, index));
    SetPathLabel(w.paths[index], path.isEmpty() ? QObject::tr("No texture imported")
                                                : QDir::toNativeSeparators(path));
    SetPreview(w.previews[index], path);
  }
}

void ShowActiveStatus(const TabContext& ctx, const CannonWidgets& w) {
  const int active = ctx.model.Int("vr_cannon_texture_slot");
  w.status->setText(active <= 0
                        ? QObject::tr("Default is active. PrimedGun cannon overrides are clear.")
                        : QObject::tr("%1 is active.").arg(SlotName(active)));
}

// Copies the slot into the pack and records it as the active one.
bool ApplyAndRecord(TabContext& ctx, int slot) {
  std::string error;
  if (!Cannon::ApplySlot(ctx.paths.cannon, slot, error)) {
    QMessageBox::critical(ctx.window, QObject::tr("Cannon Textures"), QString::fromStdString(error));
    return false;
  }
  ctx.model.SetInt("vr_cannon_texture_slot", slot);
  if (ctx.saveKeyNow) {
    ctx.saveKeyNow("vr_cannon_texture_slot");
  }
  return true;
}

} // namespace

void BuildCannonTab(TabContext& ctx, QTabWidget* tabs) {
  Cannon::SeedLibrary(ctx.paths.cannon);

  QVBoxLayout* layout = ctx.MakeScrollTab(tabs, QObject::tr("Cannon Textures"));
  layout->addWidget(ctx.Section(QObject::tr("Custom Cannon Textures")));
  auto* note = new QLabel(QObject::tr(
      "PrimedGun keeps its cannon texture slots in its own folder and applies one by copying it "
      "into the game's user texture pack. Default removes the PrimedGun override so any installed "
      "HD texture pack can supply the cannon textures."));
  note->setWordWrap(true);
  note->setObjectName(QStringLiteral("PrimedGunMuted"));
  layout->addWidget(note);
  ctx.Separator(layout);

  auto w = std::make_shared<CannonWidgets>();
  w->slotGroup = new QButtonGroup(note);
  w->slotGroup->setExclusive(true);
  auto* slotRow = new QHBoxLayout;
  slotRow->setSpacing(8);
  for (int slot = 0; slot < Cannon::kSlotCount; ++slot) {
    auto* radio = new QRadioButton(SlotName(slot));
    w->slotGroup->addButton(radio, slot);
    slotRow->addWidget(radio);
  }
  slotRow->addStretch();
  layout->addLayout(slotRow);

  w->status = new QLabel;
  w->status->setWordWrap(true);
  w->status->setObjectName(QStringLiteral("PrimedGunMuted"));
  w->status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  w->status->setMinimumWidth(220);
  layout->addWidget(w->status);

  auto* box = new QGroupBox(QObject::tr("Selected Slot Files"));
  auto* grid = new QGridLayout(box);
  grid->setColumnStretch(2, 1);
  for (int index = 0; index < 3; ++index) {
    auto* target = new QLabel(QObject::tr(Cannon::kTextureLabels[index].data()), box);
    auto* path = new QLabel(box);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setObjectName(QStringLiteral("PrimedGunMuted"));
    path->setWordWrap(true);
    path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    path->setMinimumWidth(220);
    auto* preview = new QLabel(box);
    preview->setFixedSize(72, 72);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFrameShape(QFrame::StyledPanel);
    preview->setObjectName(QStringLiteral("PrimedGunMuted"));
    auto* import = new QPushButton(QObject::tr("Import..."), box);
    grid->addWidget(target, index, 0);
    grid->addWidget(preview, index, 1);
    grid->addWidget(path, index, 2);
    grid->addWidget(import, index, 3);
    w->paths[index] = path;
    w->previews[index] = preview;
    w->imports[index] = import;

    QObject::connect(import, &QPushButton::clicked, import, [&ctx, w, index] {
      const int slot = w->slotGroup->checkedId();
      if (slot != Cannon::kCustomSlot) {
        QMessageBox::information(ctx.window, QObject::tr("Cannon Textures"),
                                 QObject::tr("Choose Custom before importing a texture."));
        return;
      }
      const QString customDir = FromPath(ctx.paths.cannon.library / "custom");
      QDir().mkpath(customDir);
      const QString source = QFileDialog::getOpenFileName(
          ctx.window, QObject::tr("Select Cannon Texture"), customDir,
          QObject::tr("Texture Images (*.png *.dds);;All Files (*)"));
      if (source.isEmpty()) {
        return;
      }
      std::string error;
      const auto destination =
          Cannon::ImportIntoSlot(ctx.paths.cannon, slot, index, ToPath(source), error);
      if (destination.empty()) {
        QMessageBox::warning(ctx.window, QObject::tr("Cannon Textures"),
                             QString::fromStdString(error));
        return;
      }
      RefreshRows(ctx, *w);
      w->status->setText(
          QObject::tr("Imported texture for %1. Click Apply to use it.").arg(SlotName(slot)));
    });
  }
  layout->addWidget(box);

  auto* actions = new QHBoxLayout;
  auto* apply = new QPushButton(QObject::tr("Apply"));
  auto* removeShine = new QPushButton(QObject::tr("Remove Shine"));
  auto* restoreShine = new QPushButton(QObject::tr("Restore Shine"));
  auto* openLibrary = new QPushButton(QObject::tr("Open Slot Folder"));
  auto* openPack = new QPushButton(QObject::tr("Open Active Pack"));
  for (QPushButton* button : {apply, removeShine, restoreShine, openLibrary, openPack}) {
    actions->addWidget(button);
  }
  actions->addStretch();
  layout->addLayout(actions);
  layout->addStretch();

  QObject::connect(w->slotGroup, &QButtonGroup::idClicked, w->slotGroup, [&ctx, w](int slot) {
    RefreshRows(ctx, *w);
    w->status->setText(slot <= 0 ? QObject::tr("Default selected. Click Apply to use it.")
                                 : QObject::tr("%1 selected. Click Apply to use it.")
                                       .arg(SlotName(slot)));
  });
  QObject::connect(apply, &QPushButton::clicked, apply, [&ctx, w] {
    const int slot = w->slotGroup->checkedId();
    if (!ApplyAndRecord(ctx, slot)) {
      return;
    }
    RefreshRows(ctx, *w);
    w->status->setText(slot <= 0
                           ? QObject::tr("Default applied. Installed HD texture packs can supply "
                                         "the cannon.")
                           : QObject::tr("Applied %1. A running game shows it at once.")
                                 .arg(SlotName(slot)));
  });
  QObject::connect(removeShine, &QPushButton::clicked, removeShine, [&ctx, w] {
    const int slot = w->slotGroup->checkedId();
    if (slot <= 0) {
      QMessageBox::information(ctx.window, QObject::tr("Cannon Textures"),
                               QObject::tr("Choose Slot 1-4 or Custom before applying Remove Shine."));
      return;
    }
    std::string error;
    if (Cannon::RemoveShine(ctx.paths.cannon, slot, error).empty()) {
      QMessageBox::critical(ctx.window, QObject::tr("Cannon Textures"), QString::fromStdString(error));
      return;
    }
    if (!ApplyAndRecord(ctx, slot)) {
      return;
    }
    RefreshRows(ctx, *w);
    w->status->setText(QObject::tr("Applied %1 with Remove Shine.").arg(SlotName(slot)));
  });
  QObject::connect(restoreShine, &QPushButton::clicked, restoreShine, [&ctx, w] {
    const int slot = w->slotGroup->checkedId();
    if (slot <= 0) {
      QMessageBox::information(ctx.window, QObject::tr("Cannon Textures"),
                               QObject::tr("Choose Slot 1-4 or Custom before restoring shine."));
      return;
    }
    std::string error;
    if (Cannon::RestoreShine(ctx.paths.cannon, slot, error).empty()) {
      QMessageBox::critical(ctx.window, QObject::tr("Cannon Textures"), QString::fromStdString(error));
      return;
    }
    if (!ApplyAndRecord(ctx, slot)) {
      return;
    }
    RefreshRows(ctx, *w);
    w->status->setText(QObject::tr("Restored shine for %1. Cannon base textures are unchanged.")
                           .arg(SlotName(slot)));
  });
  QObject::connect(openLibrary, &QPushButton::clicked, openLibrary,
                   [&ctx] { OpenFolder(FromPath(ctx.paths.cannon.library)); });
  QObject::connect(openPack, &QPushButton::clicked, openPack,
                   [&ctx] { OpenFolder(ctx.paths.userTextures); });

  ctx.AddRefresher([&ctx, w] {
    const int active = ctx.model.Int("vr_cannon_texture_slot");
    if (QAbstractButton* button = w->slotGroup->button(active)) {
      button->setChecked(true);
    }
    RefreshRows(ctx, *w);
    ShowActiveStatus(ctx, *w);
  });
}

} // namespace PrimedGunLauncher
