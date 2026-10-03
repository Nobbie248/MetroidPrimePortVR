// SPDX-License-Identifier: GPL-3.0-or-later
//
// The About tab of PrimedGun's Quest launcher (PrimedGunAboutFragment.kt,
// primedgun_strings.xml), for the native build: version, folders, links and
// credits, with the native port's own lineage added.

#include "tabs.h"

#include "tab_context.h"

#include "port_build_info.h"

#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace PrimedGunLauncher {
namespace {

QLabel* Paragraph(const QString& text, bool muted = false) {
  auto* label = new QLabel(text);
  label->setWordWrap(true);
  label->setTextFormat(Qt::RichText);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  if (muted) {
    label->setObjectName(QStringLiteral("PrimedGunMuted"));
  }
  return label;
}

QString Bullets(const QStringList& items) {
  QString html = QStringLiteral("<ul style=\"margin: 0; padding-left: 22px;\">");
  for (const QString& item : items) {
    html += QStringLiteral("<li style=\"margin-bottom: 6px;\">") + item.toHtmlEscaped() +
            QStringLiteral("</li>");
  }
  return html + QStringLiteral("</ul>");
}

} // namespace

void BuildAboutTab(TabContext& ctx, QTabWidget* tabs) {
  QVBoxLayout* layout = ctx.MakeScrollTab(tabs, QObject::tr("About"));
  layout->addWidget(ctx.Section(QObject::tr("PrimedGun")));
  layout->addWidget(Paragraph(QObject::tr(
      "Metroid Prime as a native VR game, built on the Metroid Prime decompilation's PC port.")));
  layout->addWidget(Paragraph(
      QObject::tr("Version: %1").arg(QStringLiteral(PRIMEDGUN_LAUNCHER_VERSION)), true));
  layout->addWidget(
      Paragraph(QObject::tr("Build: revision %1").arg(QStringLiteral(MP_BUILD_REVISION)), true));
  layout->addWidget(Paragraph(
      QObject::tr("Supported game: Metroid Prime NTSC-U Revision 0 (1.0), game ID GM8E01"), true));
  layout->addWidget(Paragraph(
      QObject::tr("User folder: %1").arg(QDir::toNativeSeparators(ctx.paths.userFolder).toHtmlEscaped()),
      true));
  layout->addWidget(Paragraph(
      QObject::tr("The PrimedGun launcher is free software, licensed under the GNU GPL version 3 or "
                  "later."),
      true));

  layout->addSpacing(8);
  layout->addWidget(ctx.Section(QObject::tr("Useful Information")));
  layout->addWidget(Paragraph(Bullets({
      QObject::tr("Leave the game from its own menu, by closing its window, or with Stop on the "
                  "Setup tab. The game saves its settings and shader caches as it exits."),
      QObject::tr("The first start after an update compiles shaders in the background; later starts "
                  "are faster."),
      QObject::tr("Your memory card is in USA/Card A beside the game, and the settings in "
                  "port_settings.ini in the user folder shown above."),
  })));

  layout->addSpacing(8);
  layout->addWidget(ctx.Section(QObject::tr("Links")));
  auto* links = new QHBoxLayout;
  auto* github = new QPushButton(QObject::tr("PrimedGun on GitHub"));
  auto* discord = new QPushButton(QObject::tr("Dolphin VR Discord"));
  links->addWidget(github);
  links->addWidget(discord);
  links->addStretch();
  layout->addLayout(links);
  layout->addWidget(Paragraph(
      QObject::tr("For further enhancements to your VR experience, join the Dolphin VR Discord."),
      true));
  QObject::connect(github, &QPushButton::clicked, github, [] {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/Nobbie248/PrimedGun")));
  });
  QObject::connect(discord, &QPushButton::clicked, discord, [] {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://discord.gg/GdmffzCTrh")));
  });

  layout->addSpacing(8);
  layout->addWidget(ctx.Section(QObject::tr("Credits")));
  layout->addWidget(Paragraph(Bullets({
      QObject::tr("Created by Nobbie."),
      QObject::tr("Huge thank you to iChris4, who made Dolphin ReduX for PC and Quest, ported "
                  "PrimedGun to the Quest, and early on helped fix the visor effects and ported the "
                  "Metroid Prime override from the old Dolphin Hydra."),
      QObject::tr("Thank you to the Metroid Prime modding community for the resources and research "
                  "that helped make this possible."),
      QObject::tr("Development references include the Metroid Prime decompilation project, "
                  "Metaforce, and PrimeHack. Codex was used to assist with debugging, system "
                  "integration, and iterative development."),
      QObject::tr("Thank you to the Dolphin team."),
      QObject::tr("Thank you to the early testers: GeekyGami, Lucaspec72, TorchRing, detective_yoshi, "
                  "PHA3ESH1FTGAMES, retrovideogamer, Samevi, Mochu, VideoGameEsoterica and VRified "
                  "Games."),
      QObject::tr("Thank you to budwheizzah for helping test the Quest version."),
  })));

  layout->addSpacing(8);
  layout->addWidget(ctx.Section(QObject::tr("Native Port")));
  layout->addWidget(Paragraph(Bullets({
      QObject::tr("PrimedGun v2 runs on a native PC build of the Metroid Prime decompilation "
                  "(PrimeDecomp), through Odran's MetroidPrimePort, drawn by Aurora."),
      QObject::tr("Its OpenXR stereo rendering, head tracking and PrimedGun controls build on the "
                  "OpenXR work from Wiicompiled VR."),
  })));
  layout->addStretch();
}

} // namespace PrimedGunLauncher
