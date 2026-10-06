// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2.launcher

import java.util.Locale

/**
 * The launcher's working copy of port_settings.ini, held natively by the PC
 * launcher's SettingsModel ([LauncherNative]). Edits stay in memory until Save
 * Settings or Play writes the changed keys, and only those, over the file as it
 * is then; every other line stays as the game wrote it.
 *
 * Each key's kind, default, slider range and "active" flag come from
 * launcher/core/launcher_keys.cpp, the table the PC launcher and its tests use.
 * A key that is not active is one the game saves but does not read yet; its
 * row carries a "not active yet" tag.
 */
object LauncherSettings {

    enum class Kind { BOOL, FLOAT, INT, CHOICE }

    class Key(
        val name: String,
        val kind: Kind,
        val default: String,
        val min: Float,
        val max: Float,
        val step: Float,
        val active: Boolean,
        val resetAll: Boolean,
        val choices: List<String>,
    )

    private val keys: Map<String, Key> by lazy {
        LauncherNative.keyTable().associate { row ->
            val f = row.split('\t')
            f[0] to Key(
                name = f[0],
                kind = Kind.valueOf(f[1].uppercase(Locale.ROOT)),
                default = f[2],
                min = f[3].toFloat(),
                max = f[4].toFloat(),
                step = f[5].toFloat(),
                active = f[6] == "1",
                resetAll = f[7] == "1",
                choices = f.getOrNull(8)?.split(',')?.filter { it.isNotEmpty() } ?: emptyList(),
            )
        }
    }

    fun key(name: String): Key = keys[name] ?: error("$name is not a launcher key (launcher_keys.cpp)")

    fun isActive(name: String): Boolean = keys[name]?.active ?: true

    fun string(name: String): String = LauncherNative.value(name)

    fun bool(name: String): Boolean = string(name).let { it == "1" || it == "true" || it == "on" || it == "yes" }

    fun float(name: String): Float = string(name).toFloatOrNull() ?: key(name).default.toFloat()

    fun int(name: String): Int = string(name).toIntOrNull() ?: key(name).default.toInt()

    fun set(name: String, value: String) = LauncherNative.set(name, value)

    fun setBool(name: String, value: Boolean) = set(name, if (value) "1" else "0")

    // The game reads floats with the C locale; a comma would be lost.
    fun setFloat(name: String, value: Float) = set(name, String.format(Locale.ROOT, "%.4f", value).trimEnd('0').trimEnd('.'))

    fun setInt(name: String, value: Int) = set(name, value.toString())

    fun reset(vararg names: String) = names.forEach(LauncherNative::resetToDefault)

    val dirty: Boolean get() = LauncherNative.dirty()

    /** Writes the changed keys; null on success, else why not. */
    fun save(): String? = LauncherNative.save().ifEmpty { null }

    fun saveKey(name: String): String? = LauncherNative.saveKey(name).ifEmpty { null }
}
