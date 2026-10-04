// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2

import android.content.Context
import dev.encounter.aurora.AuroraSurface

/**
 * Aurora's surface with its buffer pinned to 1280x720. SDL sizes the Android
 * surface to the whole display (4128x2208 on a Quest 3), which nobody sees while
 * OpenXR drives the headset, and Aurora presents into it every frame. At the
 * default EFB scale the game's own render and the virtual screen do not depend
 * on it; with auto scale (render_scale 0) the EFB would follow it. Wiicompiled
 * VR's QuestSurface does the same.
 *
 * It stays an AuroraSurface: Aurora's JNI natives are bound to that class name.
 */
class QuestSurface(context: Context) : AuroraSurface(context) {
    init {
        holder.setFixedSize(SURFACE_WIDTH, SURFACE_HEIGHT)
    }

    private companion object {
        const val SURFACE_WIDTH = 1280
        const val SURFACE_HEIGHT = 720
    }
}
