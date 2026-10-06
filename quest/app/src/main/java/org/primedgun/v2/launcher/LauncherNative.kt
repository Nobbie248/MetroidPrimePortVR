// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2.launcher

/**
 * The PC launcher's Qt-free core (launcher/core) in libprimedgun_launcher.so
 * (launcher/jni/primedgun_launcher_jni.cpp): the key table, the settings model that
 * edits port_settings.ini line by line, the disc check, the cannon texture pack
 * and the PrimedGun.ini import. Calls are serialised natively; the file work is
 * small, so the UI thread may call it, except where noted.
 *
 * Only the launcher process loads this library. The game process never does.
 */
object LauncherNative {
    init {
        System.loadLibrary("primedgun_launcher")
    }

    /** Sets the game's user folder and the APK's cannon slots; reads the settings. */
    @JvmStatic external fun init(userFolder: String, shippedCannonLibrary: String): Boolean

    @JvmStatic external fun buildRevision(): String

    /** One tab-separated row per key; see [LauncherKeys]. */
    @JvmStatic external fun keyTable(): Array<String>

    @JvmStatic external fun reload(): Boolean

    /** The file differs from what the launcher last read or wrote (the game rewrote it). */
    @JvmStatic external fun changedOnDisk(): Boolean

    @JvmStatic external fun value(key: String): String

    @JvmStatic external fun set(key: String, value: String)

    @JvmStatic external fun resetToDefault(key: String)

    @JvmStatic external fun resetAll()

    @JvmStatic external fun dirty(): Boolean

    /** Writes the changed keys; returns the error, or "". */
    @JvmStatic external fun save(): String

    /** Writes [key] alone; returns the error, or "". */
    @JvmStatic external fun saveKey(key: String): String

    @JvmStatic external fun isSupportedDiscName(name: String): Boolean

    /** check|gameId|revision for the first [length] bytes of a disc image. */
    @JvmStatic external fun probeDisc(extension: String, header: ByteArray, length: Int): String

    @JvmStatic external fun cannonLibraryFolder(): String

    @JvmStatic external fun cannonPackFolders(): Array<String>

    @JvmStatic external fun cannonSource(slot: Int, index: Int): String

    @JvmStatic external fun cannonApply(slot: Int): String

    @JvmStatic external fun cannonImport(slot: Int, index: Int, source: String): String

    @JvmStatic external fun cannonRemoveShine(slot: Int): String

    @JvmStatic external fun cannonRestoreShine(slot: Int): String

    /** {width, height, ARGB...} of a DXT1 .dds, or null. */
    @JvmStatic external fun decodeDds(path: String): IntArray?

    /** The settings file found under an old install's user folder, then key=value entries. */
    @JvmStatic external fun readOldSettings(userFolder: String): Array<String>
}
