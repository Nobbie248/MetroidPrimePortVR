// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from PrimedGun's Quest launcher (features/primedgun/ui/PrimedGunItem.kt,
// Copyright 2026 PrimedGun Project, GPL-2.0-or-later).
package org.primedgun.v2.launcher

/**
 * One row of the PrimedGun settings list, matching the control vocabulary of the Qt
 * launcher.
 *
 * Rows carry getter/setter lambdas rather than a settings key so that derived values
 * (the signed HUD axes) and inverted ones (a radio pair whose second option is the
 * `true` case) need no special handling in the adapter. [inactive] marks a setting
 * the game saves but does not use yet: the row shows a "not active yet" tag.
 */
sealed class PrimedGunItem {
    /** Orange section title, e.g. "Directional Movement". */
    class Header(val title: String) : PrimedGunItem()

    /** Muted explanatory line, used where the Qt window shows a tooltip or a note label. */
    class Note(val text: String) : PrimedGunItem()

    /** Full-width button, e.g. "Reset Controller". [edits] is false for one that changes no setting. */
    class Action(val title: String, val edits: Boolean = true, val onClick: () -> Unit) : PrimedGunItem()

    class Switch(
        val title: String,
        val description: String? = null,
        val inactive: Boolean = false,
        /** The list re-reads every row after an edit: another row shows the same key. */
        val refreshes: Boolean = false,
        val get: () -> Boolean,
        val set: (Boolean) -> Unit
    ) : PrimedGunItem()

    class Slider(
        val title: String,
        val min: Float,
        val max: Float,
        val step: Float,
        val inactive: Boolean = false,
        val description: String? = null,
        val format: ((Float) -> String)? = null,
        val refreshes: Boolean = false,
        /** False greys the row out, as the Qt window disables the EFB scale under Auto. */
        val enabled: () -> Boolean = { true },
        val get: () -> Float,
        val set: (Float) -> Unit
    ) : PrimedGunItem() {
        /** Matches the Qt spin box, which shows three decimals only for sub-0.1 steps. */
        val decimals: Int get() = if (step < 0.1f) 3 else 2
    }

    /** Drop-down of mutually exclusive values, e.g. "Rumble target". */
    class Choice(
        val title: String,
        val labels: List<String>,
        val values: List<String>,
        val inactive: Boolean = false,
        val description: String? = null,
        val get: () -> String,
        val set: (String) -> Unit
    ) : PrimedGunItem()

    /** Exclusive pair of radio buttons; [get] returns true when option B is selected. */
    class Toggle2(
        val title: String? = null,
        val labelA: String,
        val labelB: String,
        val inactive: Boolean = false,
        val get: () -> Boolean,
        val set: (Boolean) -> Unit
    ) : PrimedGunItem()
}
