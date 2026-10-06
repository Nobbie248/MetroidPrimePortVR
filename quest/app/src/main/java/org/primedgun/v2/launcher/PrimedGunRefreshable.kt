// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from PrimedGun's Quest launcher (features/primedgun/ui/PrimedGunRefreshable.kt,
// Copyright 2026 PrimedGun Project, GPL-2.0-or-later).
package org.primedgun.v2.launcher

/**
 * A launcher tab whose widgets mirror settings. The launcher calls [refresh] after
 * something rewrote many settings at once (Reset All, a PrimedGun settings import, the
 * game's own rewrite of port_settings.ini) or when the game starts or stops, so the
 * visible tab re-reads them and its locked state instead of showing stale values.
 */
interface PrimedGunRefreshable {
    fun refresh()
}
