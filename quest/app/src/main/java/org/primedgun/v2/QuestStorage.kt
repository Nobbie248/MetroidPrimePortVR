// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2

import android.content.Context
import java.io.File
import java.io.RandomAccessFile

/**
 * Where the Quest build keeps the player's things, for the game process and the
 * launcher alike (each resolves the same paths; they share no memory).
 *
 * The user folder is the app's external files folder,
 * /sdcard/Android/data/org.primedgun.v2/files, as the Dolphin-based PrimedGun
 * used: a computer or adb can reach it without a special permission. The game
 * gets it as MP_USER_PATH (platform/include/port_paths.h), so port_settings.ini,
 * the memory card (USA/Card A), save states, user textures and the log all live
 * there. The game's caches stay in the private files folder.
 */
object QuestStorage {
    const val DISC_FILE = "disc.iso"
    const val LAST_ERROR_FILE = "last_error.txt"

    fun userFolder(context: Context): File {
        val folder = context.getExternalFilesDir(null) ?: context.filesDir
        folder.mkdirs()
        return folder
    }

    /** The disc image the launcher copies in, the one the game always plays. */
    fun discImage(context: Context): File = File(userFolder(context), DISC_FILE)

    /** Why the last session could not start or ended, for the launcher to show. */
    fun lastErrorFile(context: Context): File = File(userFolder(context), LAST_ERROR_FILE)

    fun writeLastError(context: Context, message: String) {
        runCatching { lastErrorFile(context).writeText(message.trim() + "\n") }
    }

    fun clearLastError(context: Context) {
        runCatching { lastErrorFile(context).delete() }
    }

    /** The game's own log (MP_LOG_FILE), rewritten at every start. */
    fun gameLog(context: Context): File = File(userFolder(context), "metroid_prime_port.log")

    /** The game's settings, which the launcher edits between runs. */
    fun settingsFile(context: Context): File = File(userFolder(context), "port_settings.ini")

    /** The memory card the game mounts (DolphinCMemoryCardSys: <user>/USA/Card A). */
    fun cardFolder(context: Context): File = File(userFolder(context), "USA/Card A")

    /**
     * A memory card the launcher picked, which the game imports into [cardFolder] at
     * its next start (PortGci::ImportPending), then reports in [importReportFile].
     */
    fun pendingImportFolder(context: Context): File = File(userFolder(context), "primedgun/pending_import")

    fun importReportFile(context: Context): File = File(userFolder(context), "primedgun/import_report.txt")

    /** The cannon texture slots the APK carries, unpacked by the launcher. */
    fun shippedCannonLibrary(context: Context): File = File(context.filesDir, "cannon_textures")

    /**
     * Null when [file] holds Metroid Prime (USA) disc 0 revision 0, else what is
     * wrong with it. The disc header is read where launcher/core/disc_probe.cpp
     * reads it: offset 0 for a plain or NKit image, after the RVZ/WIA header, or
     * in a CISO's first block. The game checks the mounted disc again.
     */
    fun checkDisc(file: File): String? {
        if (!file.isFile || file.length() == 0L) {
            return "No game disc: select Metroid Prime (USA) in the launcher."
        }
        val probe = ByteArray(CISO_DATA_OFFSET + 8)
        val read = try {
            RandomAccessFile(file, "r").use { input -> readFully(input, probe) }
        } catch (e: Exception) {
            return "The game disc cannot be read: ${e.message}"
        }
        // WBFS keeps disc slot 0's header copy at its second hard-disk sector
        // (launcher/core/disc_probe.cpp); without a readable one, the game checks
        // the mounted disc.
        if (read >= 4 && probe.startsWith("WBFS")) {
            val sectorShift = if (read > 12) probe[8].toInt() and 0xFF else 0
            if (sectorShift !in 9..14 || probe[12].toInt() == 0 || read < (1 shl sectorShift) + 8) {
                return null
            }
            return checkHeader(probe, 1 shl sectorShift)
        }
        val headerOffset = when {
            read >= 4 && (probe.startsWith("RVZ\u0001") || probe.startsWith("WIA\u0001")) -> WIA_DISC_HEADER_OFFSET
            read >= 4 && probe.startsWith("CISO") -> if (probe[8].toInt() != 0) CISO_DATA_OFFSET else -1
            else -> 0
        }
        if (headerOffset < 0 || read < headerOffset + 8) {
            return "The game disc is truncated or not a GameCube disc image."
        }
        return checkHeader(probe, headerOffset)
    }

    private fun checkHeader(probe: ByteArray, offset: Int): String? {
        val gameId = String(probe, offset, 6, Charsets.US_ASCII)
        val disc = probe[offset + 6].toInt()
        val revision = probe[offset + 7].toInt()
        return when {
            gameId != "GM8E01" || disc != 0 -> "The selected disc is not Metroid Prime (USA) (game id $gameId)."
            revision != 0 -> "The selected disc is revision $revision; PrimedGun v2 needs revision 0 (1.00)."
            else -> null
        }
    }

    private fun readFully(input: RandomAccessFile, buffer: ByteArray): Int {
        var total = 0
        while (total < buffer.size) {
            val count = input.read(buffer, total, buffer.size - total)
            if (count < 0) {
                break
            }
            total += count
        }
        return total
    }

    private fun ByteArray.startsWith(magic: String): Boolean =
        size >= magic.length && magic.indices.all { this[it] == magic[it].code.toByte() }

    private const val WIA_DISC_HEADER_OFFSET = 0x48 + 0x10
    private const val CISO_DATA_OFFSET = 0x8000
}
