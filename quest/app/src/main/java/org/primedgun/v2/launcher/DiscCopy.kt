// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2.launcher

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.util.Log
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.Locale
import java.util.concurrent.atomic.AtomicReference
import org.primedgun.v2.QuestStorage
import org.primedgun.v2.R

/**
 * Select Game on the Quest: the picked disc image is copied to the user folder's
 * disc.iso, the one file the game plays (PrimedGunVrActivity hands it over as
 * MP_DISC), since the game process cannot open the picker's document itself. The
 * name says nothing of the format: the disc reader (nod) goes by the content, as it
 * does for upstream's phone build, which copies picks to the same name.
 *
 * Before copying, the image's first bytes go through the PC launcher's disc check
 * (Metroid Prime NTSC-U revision 0, in a format the port reads), so a wrong pick costs
 * no copy. The copy goes to disc.iso.part and is renamed over disc.iso when complete,
 * so an interrupted copy never replaces a good disc, and the copy passes the game's own
 * check before it does. [DiscCopyService] runs it in the foreground; the state lives
 * here, in the launcher process, for the Setup tab, which shows a finished copy's
 * result once even when it was not open as the copy ended.
 */
object DiscCopy {

    sealed class State {
        data object Idle : State()
        data class Copying(val name: String, val done: Long, val total: Long) : State() {
            val percent: Int get() = if (total > 0) (done * 100 / total).toInt() else 0
        }
        data class Done(val name: String) : State()
        data class Failed(val message: String) : State()
    }

    private const val TAG = "PrimedGunLauncher"
    private const val PREFERENCES = "launcher"
    private const val KEY_DISC_NAME = "disc_name"
    // A CISO keeps its first data block after a 0x8000 header; disc_probe.h.
    private const val PROBE_BYTES = 0x8008

    private val main = Handler(Looper.getMainLooper())
    private val listeners = mutableListOf<(State) -> Unit>()

    @Volatile
    var state: State = State.Idle
        private set

    // The last finished copy's result, until the Setup tab has shown it: whichever of
    // its listener and its resume replay claims it first shows it, once.
    private val unseen = AtomicReference<State?>(null)

    val isRunning: Boolean get() = state is State.Copying

    fun addListener(listener: (State) -> Unit) {
        listeners += listener
    }

    fun removeListener(listener: (State) -> Unit) {
        listeners -= listener
    }

    private fun publish(next: State) {
        state = next
        if (next is State.Done || next is State.Failed) {
            unseen.set(next)
        }
        main.post { listeners.toList().forEach { it(next) } }
    }

    /** The last finished copy's result while nobody has shown it, else null. */
    fun pendingResult(): State? = unseen.get()

    /** True for the one caller that gets to show [result]. */
    fun claimResult(result: State): Boolean = unseen.compareAndSet(result, null)

    /**
     * Drops a disc.iso.part a copy left when its process died (force stop, low memory,
     * a reboot) and no catch ran. In a new launcher process no copy is running.
     */
    fun discardStalePartial(context: Context) {
        if (!isRunning) {
            File(QuestStorage.discImage(context).path + ".part").delete()
        }
    }

    /** The name of the file last copied in, for "Selected: ...". */
    fun selectedName(context: Context): String? =
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).getString(KEY_DISC_NAME, null)

    private fun setSelectedName(context: Context, name: String?) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit().apply {
            if (name == null) remove(KEY_DISC_NAME) else putString(KEY_DISC_NAME, name)
        }.apply()
    }

    /** Deletes the copy (Game Options > Remove Game Copy). */
    fun forget(context: Context) {
        QuestStorage.discImage(context).delete()
        File(QuestStorage.discImage(context).path + ".part").delete()
        setSelectedName(context, null)
    }

    fun displayName(context: Context, uri: Uri): String? =
        runCatching {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0) else null
            }
        }.getOrNull() ?: uri.lastPathSegment?.substringAfterLast('/')

    private fun size(context: Context, uri: Uri): Long =
        runCatching {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst() && !cursor.isNull(0)) cursor.getLong(0) else -1L
            }
        }.getOrNull() ?: -1L

    /**
     * Why [uri] cannot be the game disc, from its name and first bytes, or null when
     * it can (or is a format checked only at boot). Reads the document; not on the UI
     * thread.
     */
    fun check(context: Context, uri: Uri, name: String): String? {
        if (!LauncherNative.isSupportedDiscName(name)) {
            return context.getString(R.string.primedgun_disc_unsupported_format, name)
        }
        val header = ByteArray(PROBE_BYTES)
        val read = try {
            context.contentResolver.openInputStream(uri)?.use { readUpTo(it, header) }
                ?: return context.getString(R.string.primedgun_disc_unreadable, name)
        } catch (e: Exception) {
            // IOException, a revoked grant, or a provider's own runtime failure.
            return context.getString(R.string.primedgun_disc_unreadable, name)
        }
        val extension = name.substringAfterLast('.', "").let { if (it.isEmpty()) "" else "." + it.lowercase(Locale.ROOT) }
        val (check, gameId, revision) = LauncherNative.probeDisc(extension, header, read).split('|')
        return when (check) {
            "ok", "unverified" -> null
            "wrong_game" -> context.getString(R.string.primedgun_disc_wrong_game, gameId.ifEmpty { "?" })
            "wrong_revision" -> context.getString(R.string.primedgun_disc_wrong_revision, revision)
            "unsupported_format" -> context.getString(R.string.primedgun_disc_unsupported_format, name)
            else -> context.getString(R.string.primedgun_disc_unreadable, name)
        }
    }

    /** Checks and copies [uri] into disc.iso; runs on [DiscCopyService]'s thread. */
    fun run(context: Context, uri: Uri) {
        val name = displayName(context, uri) ?: "disc"
        unseen.set(null)
        publish(State.Copying(name, 0, size(context, uri)))
        val target = QuestStorage.discImage(context)
        val partial = File(target.path + ".part")
        try {
            val problem = check(context, uri, name)
            if (problem != null) {
                publish(State.Failed(problem))
                return
            }
            val total = size(context, uri)
            val input = context.contentResolver.openInputStream(uri)
                ?: throw IOException(context.getString(R.string.primedgun_disc_unreadable, name))
            input.use { source ->
                partial.outputStream().use { output ->
                    val buffer = ByteArray(1 shl 20)
                    var done = 0L
                    var lastPublished = 0L
                    while (true) {
                        val count = source.read(buffer)
                        if (count < 0) break
                        output.write(buffer, 0, count)
                        done += count
                        if (done - lastPublished >= (16L shl 20)) {
                            lastPublished = done
                            publish(State.Copying(name, done, total))
                        }
                    }
                    // A provider can end a dropped stream with EOF rather than an error.
                    if (total > 0 && done < total) {
                        throw IOException("copy incomplete: $done of $total bytes")
                    }
                    output.fd.sync()
                }
            }
            // The game's own check, on the copy, before it replaces the disc in use.
            QuestStorage.checkDisc(partial)?.let { copied ->
                partial.delete()
                publish(State.Failed(copied))
                return
            }
            target.delete()
            if (!partial.renameTo(target)) {
                throw IOException("cannot rename ${partial.path}")
            }
            setSelectedName(context, name)
            Log.i(TAG, "Copied $name to ${target.path} (${target.length()} bytes)")
            publish(State.Done(name))
        } catch (e: Exception) {
            Log.e(TAG, "Copying $name failed", e)
            partial.delete()
            publish(State.Failed(context.getString(R.string.primedgun_disc_copy_failed, e.message ?: e.toString())))
        }
    }

    private fun readUpTo(input: InputStream, buffer: ByteArray): Int {
        var total = 0
        while (total < buffer.size) {
            val count = input.read(buffer, total, buffer.size - total)
            if (count < 0) break
            total += count
        }
        return total
    }
}
