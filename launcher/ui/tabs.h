// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher's tabs after Setup (which lives in the window, beside the
// process it starts), in PrimedGun's order.

#pragma once

class QTabWidget;

namespace PrimedGunLauncher {

class TabContext;

void BuildControllerTab(TabContext& ctx, QTabWidget* tabs);
void BuildCalibrationTab(TabContext& ctx, QTabWidget* tabs);
void BuildCannonTab(TabContext& ctx, QTabWidget* tabs);
void BuildLayoutTab(TabContext& ctx, QTabWidget* tabs);
void BuildPortConfigTab(TabContext& ctx, QTabWidget* tabs);
void BuildAboutTab(TabContext& ctx, QTabWidget* tabs);

} // namespace PrimedGunLauncher
