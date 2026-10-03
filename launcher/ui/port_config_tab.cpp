// SPDX-License-Identifier: GPL-3.0-or-later
//
// In place of PrimedGun's Dolphin Config tab: the native port's own headset and
// display settings, and the folders the game writes to.

#include "tabs.h"

#include "settings_model.h"
#include "tab_context.h"
#include "widgets.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace PrimedGunLauncher {

void BuildPortConfigTab(TabContext& ctx, QTabWidget* tabs) {
  QVBoxLayout* layout = ctx.MakeScrollTab(tabs, QObject::tr("Port Config"));
  layout->addWidget(ctx.Section(QObject::tr("Port Config")));
  auto* note = new QLabel(QObject::tr(
      "The native port's own settings. In game, F1 opens the full port overlay."));
  note->setObjectName(QStringLiteral("PrimedGunMuted"));
  note->setWordWrap(true);
  layout->addWidget(note);
  layout->addSpacing(8);

  layout->addWidget(ctx.Section(QObject::tr("VR Headset")));
  ctx.Check(layout, QObject::tr("Enable VR"), "vr_enabled",
            QObject::tr("Off starts the game on the desktop only."));
  ctx.Combo(layout, QObject::tr("Control scheme"), "vr_controller_mode",
            {{QObject::tr("PrimedGun"), QStringLiteral("primedgun")},
             {QObject::tr("Gamepad"), QStringLiteral("gamepad")},
             {QObject::tr("None"), QStringLiteral("none")}});
  ctx.Combo(layout, QObject::tr("Mirror view"), "vr_mirror_view",
            {{QObject::tr("Normal"), QStringLiteral("normal")},
             {QObject::tr("Both eyes"), QStringLiteral("both")},
             {QObject::tr("Left eye"), QStringLiteral("left")},
             {QObject::tr("Right eye"), QStringLiteral("right")},
             {QObject::tr("None"), QStringLiteral("none")}});
  ctx.Float(layout, QObject::tr("Headset render scale"), "vr_render_scale")
      ->setToolTip(QObject::tr("Of the eye size the VR runtime recommends."));
  ctx.Float(layout, QObject::tr("World scale"), "vr_world_scale")
      ->setToolTip(QObject::tr("Game units per metre; a larger value makes the world look smaller."));
  ctx.Check(layout, QObject::tr("Draw the world per eye"), "vr_immersive_replay",
            QObject::tr("Off shows the flat game image on the virtual screen."));
  ctx.Check(layout, QObject::tr("Remove cinematic bars"), "vr_remove_cinematic_bars",
            QObject::tr("Keeps the morph ball and visor transitions full size in the headset."));
  ctx.Float(layout, QObject::tr("Virtual screen distance"), "vr_screen_distance_meters")
      ->setToolTip(QObject::tr("Metres; menus and cutscenes hang on this screen."));
  ctx.Float(layout, QObject::tr("Virtual screen width"), "vr_screen_width_meters")
      ->setToolTip(QObject::tr("Metres."));
  ctx.Float(layout, QObject::tr("Lean back"), "vr_lean_back_degrees")
      ->setToolTip(QObject::tr("Degrees, for playing seated or lying back."));

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Display")));
  ctx.Check(layout, QObject::tr("Fullscreen"), "fullscreen");
  ctx.Check(layout, QObject::tr("VSync"), "vsync");
  ctx.Combo(layout, QObject::tr("MSAA"), "msaa",
            {{QObject::tr("Off"), QStringLiteral("1")}, {QObject::tr("4x"), QStringLiteral("4")}});
  ctx.Combo(layout, QObject::tr("Anisotropic filtering"), "anisotropy",
            {{QObject::tr("1x"), QStringLiteral("1")},
             {QObject::tr("2x"), QStringLiteral("2")},
             {QObject::tr("4x"), QStringLiteral("4")},
             {QObject::tr("8x"), QStringLiteral("8")},
             {QObject::tr("16x"), QStringLiteral("16")}});
  // render_scale 0 is the overlay's "Auto render scale (native)".
  auto* autoScale = new QCheckBox(QObject::tr("Auto render scale (native)"));
  layout->addWidget(autoScale);
  FloatRow* scale = ctx.FloatWith(
      layout, QObject::tr("EFB scale"), 1.0, 2.0, 0.25,
      [&ctx] { return std::max(ctx.model.Float("render_scale"), 1.0f); },
      [&ctx](float v) { ctx.model.SetFloat("render_scale", v); }, "render_scale");
  scale->setToolTip(QObject::tr("Scales the internal EFB; higher values use more GPU memory."));
  QObject::connect(autoScale, &QCheckBox::toggled, autoScale, [&ctx, scale](bool checked) {
    ctx.model.SetFloat("render_scale", checked ? 0.0f : 1.0f);
    scale->SetValue(1.0);
    scale->setEnabled(!checked);
    ctx.Edited();
  });
  ctx.AddRefresher([&ctx, autoScale, scale] {
    const bool isAuto = ctx.model.Float("render_scale") <= 0.0f;
    const QSignalBlocker blocker(autoScale);
    autoScale->setChecked(isAuto);
    scale->setEnabled(!isAuto);
  });

  ctx.Separator(layout);
  layout->addWidget(ctx.Section(QObject::tr("Folders")));
  auto* openUser = new QPushButton(QObject::tr("Open User Folder"));
  auto* openSettings = new QPushButton(QObject::tr("Open Settings File"));
  auto* openLog = new QPushButton(QObject::tr("Open Last Run Log"));
  auto* openTextures = new QPushButton(QObject::tr("Open Texture Pack Folder"));
  auto* openDumps = new QPushButton(QObject::tr("Open Crash Dumps"));
  openDumps->setToolTip(QObject::tr("The game writes crash_*.dmp next to itself when it crashes."));
  for (QPushButton* button : {openUser, openSettings, openLog, openTextures, openDumps}) {
    layout->addWidget(button);
  }
  layout->addStretch();

  QObject::connect(openUser, &QPushButton::clicked, openUser,
                   [&ctx] { OpenFolder(ctx.paths.userFolder); });
  QObject::connect(openSettings, &QPushButton::clicked, openSettings,
                   [&ctx] { OpenFile(ctx.paths.settingsFile); });
  QObject::connect(openLog, &QPushButton::clicked, openLog,
                   [&ctx] { OpenFile(NewestLog(ctx.paths)); });
  QObject::connect(openTextures, &QPushButton::clicked, openTextures,
                   [&ctx] { OpenFolder(ctx.paths.userTextures); });
  QObject::connect(openDumps, &QPushButton::clicked, openDumps,
                   [&ctx] { OpenFolder(ctx.paths.exeFolder); });
}

} // namespace PrimedGunLauncher
