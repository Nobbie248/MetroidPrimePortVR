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
 * the memory card (USA/Card A, EUR/Card A for a PAL disc), save states, user
 * textures and the log all live there. The game's caches stay in the private files folder.
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

    /**
     * The memory card the game mounts (DolphinCMemoryCardSys: <user>/<region>/Card A),
     * for the copied disc's region ([cardRegion]). Reads the disc's first bytes.
     */
    fun cardFolder(context: Context): File = File(userFolder(context), "${cardRegion(context)}/Card A")

    /**
     * The copied disc's card region, as Dolphin and the game name it (PortGci::CardRegion):
     * EUR for a PAL disc (GM8P01), else USA, also with no disc or a WBFS whose header is
     * not read here. Reads the disc's first bytes (32 KiB at most).
     */
    fun cardRegion(context: Context): String {
        val probe = ByteArray(CISO_DATA_OFFSET + 8)
        val read = runCatching { RandomAccessFile(discImage(context), "r").use { readFully(it, probe) } }.getOrDefault(0)
        val offset = headerOffset(probe, read)
        return if (offset >= 0 && String(probe, offset, 6, Charsets.US_ASCII) == "GM8P01") "EUR" else "USA"
    }

    /**
     * A memory card the launcher picked, which the game imports into [cardFolder] at
     * its next start (PortGci::ImportPending), then reports in [importReportFile].
     */
    fun pendingImportFolder(context: Context): File = File(userFolder(context), "primedgun/pending_import")

    fun importReportFile(context: Context): File = File(userFolder(context), "primedgun/import_report.txt")

    /** The cannon texture slots the APK carries, unpacked by the launcher. */
    fun shippedCannonLibrary(context: Context): File = File(context.filesDir, "cannon_textures")

    /**
     * Null when [file] holds Metroid Prime USA 1.00 (GM8E01) or PAL (GM8P01,
     * experimental), disc 0 revision 0, else what is wrong with it. These are the discs
     * the game boots (platform/disc.cpp PortDisc::IsAccepted), and it checks the
     * mounted disc again.
     */
    fun checkDisc(file: File): String? {
        if (!file.isFile || file.length() == 0L) {
            return "No game disc: select Metroid Prime in the launcher."
        }
        val probe = ByteArray(CISO_DATA_OFFSET + 8)
        val read = try {
            RandomAccessFile(file, "r").use { input -> readFully(input, probe) }
        } catch (e: Exception) {
            return "The game disc cannot be read: ${e.message}"
        }
        return when (val offset = headerOffset(probe, read)) {
            UNCHECKED -> null
            TRUNCATED -> "The game disc is truncated or not a GameCube disc image."
            else -> checkHeader(probe, offset)
        }
    }

    /**
     * Where the first [read] bytes of [probe] keep the disc header, as
     * launcher/core/disc_probe.cpp reads it: offset 0 for a plain or NKit image, after
     * the RVZ/WIA header, in a CISO's first block, or in a WBFS's copy of disc slot 0's
     * header at its second hard-disk sector. [UNCHECKED] for a WBFS without a readable
     * copy (the game checks the mounted disc), [TRUNCATED] when the header is out of reach.
     */
    private fun headerOffset(probe: ByteArray, read: Int): Int {
        if (read >= 4 && probe.startsWith("WBFS")) {
            val sectorShift = if (read > 12) probe[8].toInt() and 0xFF else 0
            if (sectorShift !in 9..14 || probe[12].toInt() == 0 || read < (1 shl sectorShift) + 8) {
                return UNCHECKED
            }
            return 1 shl sectorShift
        }
        val offset = when {
            read >= 4 && (probe.startsWith("RVZ\u0001") || probe.startsWith("WIA\u0001")) -> WIA_DISC_HEADER_OFFSET
            read >= 4 && probe.startsWith("CISO") -> if (probe[8].toInt() != 0) CISO_DATA_OFFSET else TRUNCATED
            else -> 0
        }
        return if (offset < 0 || read < offset + 8) TRUNCATED else offset
    }

    private fun checkHeader(probe: ByteArray, offset: Int): String? {
        val gameId = String(probe, offset, 6, Charsets.US_ASCII)
        val disc = probe[offset + 6].toInt()
        val revision = probe[offset + 7].toInt()
        // The Setup tab tells these apart by "not Metroid Prime" and "revision".
        return when {
            (gameId != "GM8E01" && gameId != "GM8P01") || disc != 0 ->
                "The selected disc is not Metroid Prime USA or PAL (game id $gameId)."
            revision != 0 -> "The selected disc is revision $revision; PrimedGun v2 needs revision 0: USA 1.00 or PAL."
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
    // headerOffset's answers when there is no header to read.
    private const val UNCHECKED = -2
    private const val TRUNCATED = -1
}
