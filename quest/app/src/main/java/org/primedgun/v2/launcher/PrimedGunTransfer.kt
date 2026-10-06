// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2.launcher

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import java.io.File
import java.io.IOException
import java.util.Locale
import java.util.zip.ZipInputStream
import kotlin.concurrent.thread
import org.primedgun.v2.QuestStorage
import org.primedgun.v2.R

/**
 * Transfer PrimedGun Memory Card / Settings, for a headset: there is no old install
 * beside the launcher to search, and the system picker cannot open another app's
 * folder, so the player picks a file:
 *  - a memory card: Dolphin's raw image (.raw, .gcp) or a save file (.gci);
 *  - PrimedGun's settings: Config/PrimedGun.ini (or Qt.ini from older builds);
 *  - or both at once: the zip PrimedGun's (Dolphin's) Export User Data writes.
 *
 * The launcher cannot read a raw card itself (the card code is the game's, built on
 * SDL), so a card is left in the user folder's primedgun/pending_import and the game
 * imports it into its card at its next start (PortGci::ImportPending), keeping the
 * saves it replaces in USA/Card A/_replaced; the launcher then shows its report. The
 * settings become unsaved edits, as on the PC.
 *
 * A transfer runs at process level, as the disc copy does ([begin]): leaving the Setup
 * tab while a large zip unpacks neither loses its settings nor its report, which the
 * tab shows when it next starts ([consumeOutcome]).
 */
object PrimedGunTransfer {

    /** A finished transfer step, for the Setup tab. [appliedFrom] names the settings taken. */
    class Outcome(val name: String, val settingsOnly: Boolean, val result: Result, val appliedFrom: String?)

    private val main = Handler(Looper.getMainLooper())
    private val listeners = mutableListOf<(Outcome) -> Unit>()
    private var unseen: Outcome? = null

    /** A transfer is staging: Play waits, so the game never races the hand-over. */
    @Volatile
    var isStaging = false
        private set

    /** The report so far, across the card step and the settings step (main thread). */
    var report = ""

    fun addListener(listener: (Outcome) -> Unit) {
        listeners += listener
    }

    fun removeListener(listener: (Outcome) -> Unit) {
        listeners -= listener
    }

    /** The last finished step, once (main thread). */
    fun consumeOutcome(): Outcome? = unseen.also { unseen = null }

    /**
     * Stages [uri] off the main thread, then, on it, takes its settings as unsaved edits
     * and hands the [Outcome] to the listeners. False when a transfer is already running.
     */
    fun begin(context: Context, uri: Uri, name: String, settingsOnly: Boolean): Boolean {
        if (isStaging) return false
        isStaging = true
        val app = context.applicationContext
        thread(name = "PrimedGunTransfer") {
            val result = try {
                stage(app, uri, name)
            } catch (e: Throwable) {
                failure(app.getString(R.string.primedgun_transfer_copy_failed, e.message ?: e.toString()))
            }
            main.post {
                val applied = if (result.error == null) applySettings(result) else null
                // The panel saves its edits as it is left; settings that arrive after that
                // are saved here, or they would live only in this cached process. The game
                // cannot hold the file: Play is refused while staging.
                if (applied != null && !LauncherActivity.panelResumed) {
                    LauncherSettings.save()
                }
                isStaging = false
                val outcome = Outcome(name, settingsOnly, result, applied)
                unseen = outcome
                listeners.toList().forEach { it(outcome) }
            }
        }
        return true
    }

    /** Takes the old settings as unsaved edits; returns where they came from, or null. */
    private fun applySettings(result: Result): String? {
        val source = result.settingsSource ?: return null
        for ((key, value) in result.settings) {
            if (key != "vr_cannon_texture_slot") {
                LauncherSettings.set(key, value)
            }
        }
        // The slot is applied as files, so it is recorded at once.
        result.settings.firstOrNull { it.first == "vr_cannon_texture_slot" }?.let { (key, value) ->
            val slot = value.toIntOrNull() ?: 0
            if (LauncherNative.cannonApply(slot).isEmpty()) {
                LauncherSettings.set(key, slot.toString())
                LauncherSettings.saveKey(key)
            }
        }
        return source
    }

    class Result(
        val cardStaged: Boolean,
        val settingsSource: String?,
        val settings: List<Pair<String, String>>,
        val error: String?,
        /** A user-data zip that held settings but no card this recognises. */
        val zipWithoutCard: Boolean = false,
    )

    private val CARD_EXTENSIONS = setOf("raw", "gcp", "gci")
    // Dolphin's slot A card, with its block count when it is not the default size.
    private val RAW_CARD = Regex("""(^|/)gc/memorycarda\.usa(\.\d+)?\.raw$""")

    fun extensionOf(name: String): String = name.substringAfterLast('.', "").lowercase(Locale.ROOT)

    fun isTransferFile(name: String): Boolean =
        extensionOf(name).let { it in CARD_EXTENSIONS || it == "ini" || it == "zip" }

    /**
     * True when a picked card is waiting for the game's next start: staged, or claimed by
     * an import that did not finish (PortGci::ImportPending takes that one up again).
     */
    fun cardPending(context: Context): Boolean {
        val pending = QuestStorage.pendingImportFolder(context)
        val claimed = File(pending.path + ".claimed")
        return listOf(pending, claimed).any { folder -> folder.listFiles()?.any { it.isFile } == true }
    }

    /** Stages what [uri] holds. Reads and writes files; not on the UI thread. */
    fun stage(context: Context, uri: Uri, name: String): Result {
        val scratch = File(context.cacheDir, "transfer")
        scratch.deleteRecursively()
        val oldUser = File(scratch, "old_user")
        val cards = File(scratch, "cards")
        try {
            when (val extension = extensionOf(name)) {
                in CARD_EXTENSIONS -> copy(context, uri, File(cards, "card.$extension"))
                "ini" -> {
                    // ReadOldSettings looks for Config/PrimedGun.ini, or Config/Qt.ini.
                    val file = if (name.equals("Qt.ini", ignoreCase = true)) "Qt.ini" else "PrimedGun.ini"
                    copy(context, uri, File(oldUser, "Config/$file"))
                }
                "zip" -> unzipUserData(context, uri, oldUser, cards)
                else -> return failure(context.getString(R.string.primedgun_transfer_wrong_extension))
            }
            val cardStaged = stageCards(context, cards)
            val (source, settings) = readSettings(oldUser)
            if (!cardStaged && source == null) {
                return failure(
                    context.getString(
                        if (extensionOf(name) == "zip") R.string.primedgun_transfer_zip_empty
                        else R.string.primedgun_transfer_nothing_found,
                        name
                    )
                )
            }
            val zip = extensionOf(name) == "zip"
            val settingsSource = source?.let { if (zip) "$name ($it)" else name }
            return Result(cardStaged, settingsSource, settings, null, zipWithoutCard = zip && !cardStaged)
        } catch (e: Exception) {
            // Unreadable documents, revoked grants, malformed zips: a message, not a crash.
            return failure(context.getString(R.string.primedgun_transfer_copy_failed, e.message ?: e.toString()))
        } finally {
            scratch.deleteRecursively()
        }
    }

    private fun failure(message: String) = Result(false, null, emptyList(), message)

    private fun copy(context: Context, uri: Uri, target: File) {
        target.parentFile?.mkdirs()
        val input = context.contentResolver.openInputStream(uri) ?: throw IOException("cannot open $uri")
        input.use { source -> target.outputStream().use { source.copyTo(it) } }
    }

    // Dolphin's user folder export: entries relative to the user folder, perhaps under
    // one more folder. The card is GC/MemoryCardA.USA.raw (MemoryCardA.USA.<blocks>.raw
    // for another size) or the GCI folder GC/USA/Card A; the settings are
    // Config/PrimedGun.ini and Config/Qt.ini.
    private fun unzipUserData(context: Context, uri: Uri, oldUser: File, cards: File) {
        val input = context.contentResolver.openInputStream(uri) ?: throw IOException("cannot open $uri")
        var raw: ByteArray? = null
        val gci = mutableMapOf<String, ByteArray>()
        // Names without the zip's UTF-8 flag are read as Latin-1, which cannot fail
        // (Windows' own zips write them in the OEM code page); the names matched are ASCII.
        ZipInputStream(input.buffered(), Charsets.ISO_8859_1).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.isDirectory) continue
                val path = entry.name.replace('\\', '/')
                val lower = path.lowercase(Locale.ROOT)
                when {
                    lower.endsWith("config/primedgun.ini") -> write(File(oldUser, "Config/PrimedGun.ini"), zip.readBytes())
                    lower.endsWith("config/qt.ini") -> write(File(oldUser, "Config/Qt.ini"), zip.readBytes())
                    RAW_CARD.containsMatchIn(lower) -> raw = zip.readBytes()
                    lower.contains("gc/usa/card a/") && lower.endsWith(".gci") -> {
                        // Only the folder's own files, not its _replaced or backups.
                        val rest = path.substring(lower.indexOf("gc/usa/card a/") + "gc/usa/card a/".length)
                        if (!rest.contains('/')) gci[rest] = zip.readBytes()
                    }
                }
            }
        }
        // PrimedGun's slot A is the raw card by default, so it comes first, as in the PC
        // launcher's search (primedgun_import.cpp scores it above the GCI folder); the GCI
        // folder counts only when it holds Metroid Prime saves (Dolphin names them
        // 01-GM8E-<name>.gci).
        val gameGci = gci.filterKeys { it.lowercase(Locale.ROOT).contains("gm8e") }
        when {
            raw != null -> write(File(cards, "MemoryCardA.USA.raw"), raw!!)
            gameGci.isNotEmpty() -> gameGci.forEach { (file, bytes) ->
                write(File(cards, file.substringBeforeLast('.') + ".gci"), bytes)
            }
        }
    }

    private fun write(target: File, bytes: ByteArray) {
        target.parentFile?.mkdirs()
        target.writeBytes(bytes)
    }

    // Replaces whatever was waiting: one pick is one save set. Staged beside the folder
    // and renamed into place, so the game never sees half a set.
    private fun stageCards(context: Context, cards: File): Boolean {
        val files = cards.listFiles()?.filter { it.isFile } ?: return false
        if (files.isEmpty()) return false
        val pending = QuestStorage.pendingImportFolder(context)
        val staging = File(pending.path + ".tmp")
        staging.deleteRecursively()
        if (!staging.mkdirs()) throw IOException("cannot create ${staging.path}")
        for (file in files) {
            file.copyTo(File(staging, file.name), overwrite = true)
        }
        pending.deleteRecursively()
        if (!staging.renameTo(pending)) throw IOException("cannot rename ${staging.path}")
        QuestStorage.importReportFile(context).delete()
        return true
    }

    private fun readSettings(oldUser: File): Pair<String?, List<Pair<String, String>>> {
        if (!oldUser.isDirectory) return null to emptyList()
        val found = LauncherNative.readOldSettings(oldUser.path)
        if (found.isEmpty()) return null to emptyList()
        val values = found.drop(1).mapNotNull { entry ->
            val equals = entry.indexOf('=')
            if (equals <= 0) null else entry.substring(0, equals) to entry.substring(equals + 1)
        }
        return File(found[0]).name to values
    }
}
