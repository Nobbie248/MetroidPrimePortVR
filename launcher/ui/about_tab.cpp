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
  // MP_BUILD_REVISION is a char array defined in the generated port_build_info.cpp
  // (upstream 074e531a), not a literal, so QStringLiteral cannot take it.
  layout->addWidget(Paragraph(
      QObject::tr("Build: revision %1").arg(QString::fromLatin1(MP_BUILD_REVISION)), true));
  layout->addWidget(Paragraph(
      QObject::tr("Supported game: Metroid Prime USA 1.00 (GM8E01) or PAL (GM8P01, experimental), "
                  "revision 0"),
      true));
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
      QObject::tr("Your memory card is in USA/Card A (EUR/Card A for a PAL disc): Game Options > "
                  "Open Memory Card Folder shows it. The settings are in port_settings.ini in the "
                  "user folder shown above."),
  })));

  layout->addSpacing(8);
  layout->addWidget(ctx.Section(QObject::tr("Links")));
  auto* links = new QVBoxLayout;
  auto* github = new QPushButton(QObject::tr("PrimedGun 2.0 — iChris4"));
  auto* legacyGithub = new QPushButton(QObject::tr("PrimedGun Legacy — Nobbie"));
  auto* discord = new QPushButton(QObject::tr("Dolphin VR Discord"));
  auto* flat2vrDiscord = new QPushButton(QObject::tr("Flat2VR Modding Discord"));
  links->addWidget(github, 0, Qt::AlignLeft);
  links->addWidget(legacyGithub, 0, Qt::AlignLeft);
  links->addWidget(discord, 0, Qt::AlignLeft);
  links->addWidget(flat2vrDiscord, 0, Qt::AlignLeft);
  layout->addLayout(links);
  layout->addWidget(Paragraph(
      QObject::tr("Join the Discord communities for VR modding discussions and support."),
      true));
  QObject::connect(github, &QPushButton::clicked, github, [] {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/iChris4/MetroidPrimePortVR")));
  });
  QObject::connect(legacyGithub, &QPushButton::clicked, legacyGithub, [] {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/Nobbie248/PrimedGun")));
  });
  QObject::connect(discord, &QPushButton::clicked, discord, [] {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://discord.gg/GdmffzCTrh")));
  });
  QObject::connect(flat2vrDiscord, &QPushButton::clicked, flat2vrDiscord, [] {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://discord.gg/flat2vr")));
  });

  layout->addSpacing(8);
  layout->addWidget(ctx.Section(QObject::tr("Credits")));
  const auto addCredit = [layout](const QString& role, const QString& credit) {
    layout->addWidget(Paragraph(QStringLiteral("<b>%1</b><br>%2")
                                    .arg(role.toHtmlEscaped(), credit.toHtmlEscaped())
                                    .replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
    layout->addSpacing(6);
  };
  addCredit(QObject::tr("Created by"), QObject::tr("Nobbie and iChris4"));
  addCredit(QObject::tr("Quest Port & Early VR Development"),
            QObject::tr("iChris4\n"
                        "Creator of Dolphin ReduX for PC and Quest. Ported PrimedGun to Quest, "
                        "contributed early visor effect fixes, and ported the Metroid Prime override "
                        "from Dolphin Hydra."));
  addCredit(QObject::tr("Development References"),
            QObject::tr("Metroid Prime decompilation project\nMetaforce\nPrimeHack"));
  addCredit(QObject::tr("Development Assistance"),
            QObject::tr("Codex — debugging, system integration, and iterative development."));
  addCredit(QObject::tr("Early Playtesting"),
            QObject::tr("GeekyGami\nLucaspec72\nTorchRing\ndetective_yoshi\nPHA3ESH1FTGAMES\n"
                        "retrovideogamer\nSamevi\nMochu\nVideoGameEsoterica\nVRified Games"));
  addCredit(QObject::tr("Quest Playtesting"), QObject::tr("budwheizzah"));
  addCredit(QObject::tr("Special Thanks"),
            QObject::tr("The Metroid Prime modding community, for the resources and research "
                        "that helped make PrimedGun possible.\nThe Dolphin team."));

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
