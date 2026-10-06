// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from PrimedGun's Quest launcher (features/primedgun/ui/PrimedGunCannonTexturesFragment.kt,
// Copyright 2026 PrimedGun Project, GPL-2.0-or-later).
package org.primedgun.v2.launcher

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.ImageView
import android.widget.RadioButton
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.core.view.isVisible
import androidx.fragment.app.Fragment
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.io.File
import java.io.IOException
import org.primedgun.v2.R
import org.primedgun.v2.databinding.FragmentPrimedgunCannonTexturesBinding

/**
 * The Cannon Textures tab, following the Qt window: pick a slot, review and import its
 * files, then Apply. Applying copies the slot into the game's user texture pack
 * (launcher/core/cannon_textures.h, shared with the PC launcher) and records it as
 * vr_cannon_texture_slot. The desktop "Open ..." buttons can only show the folder path
 * here, since a headset has no file manager to hand the folder to.
 */
class PrimedGunCannonTexturesFragment : Fragment(), PrimedGunRefreshable {

    companion object {
        const val SLOT_DEFAULT = 0
        const val SLOT_CUSTOM = 5
        const val SLOT_COUNT = 6
        private const val SLOT_KEY = "vr_cannon_texture_slot"
        private const val PREVIEW_SIDE = 256
        private val TEXTURE_LABELS = intArrayOf(
            R.string.primedgun_cannon_texture_a,
            R.string.primedgun_cannon_texture_b,
            R.string.primedgun_cannon_shine_mask,
        )
    }

    private class TextureRow(val preview: ImageView, val noPreview: TextView, val path: TextView, val import: Button)

    private var binding: FragmentPrimedgunCannonTexturesBinding? = null
    private val rows = ArrayList<TextureRow>()
    private val radios = ArrayList<RadioButton>()
    private var selectedSlot = SLOT_DEFAULT
    // The slot the settings name as applied, as last read or written here: a resume
    // (the Import picker's return) keeps an unapplied selection, a change of it does not.
    private var appliedSlot = -1
    private var importIndex = -1
    private var importSlot = SLOT_DEFAULT

    private val launcher: LauncherActivity? get() = activity as? LauncherActivity

    private val requestTexture = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) importTexture(uri)
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?): View {
        val inflated = FragmentPrimedgunCannonTexturesBinding.inflate(inflater, container, false)
        binding = inflated
        return inflated.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val b = binding ?: return
        val context = requireContext()
        val inflater = LayoutInflater.from(context)

        radios.clear()
        for (slot in 0 until SLOT_COUNT) {
            val radio = RadioButton(context).apply {
                id = View.generateViewId()
                text = slotName(slot)
                tag = slot
                setTextColor(ContextCompat.getColor(context, R.color.primedgun_text))
                minHeight = resources.getDimensionPixelSize(R.dimen.primedgun_touch_target)
                setPadding(0, 0, resources.getDimensionPixelSize(R.dimen.primedgun_spacing), 0)
            }
            b.cannonSlotGroup.addView(radio)
            radios.add(radio)
        }
        b.cannonSlotGroup.setOnCheckedChangeListener { group, checkedId ->
            val radio = group.findViewById<RadioButton>(checkedId) ?: return@setOnCheckedChangeListener
            val slot = radio.tag as Int
            if (slot == selectedSlot) return@setOnCheckedChangeListener
            selectedSlot = slot
            refreshRows()
            setStatus(
                if (slot == SLOT_DEFAULT) getString(R.string.primedgun_cannon_default_selected)
                else getString(R.string.primedgun_cannon_slot_selected, slotName(slot))
            )
        }

        rows.clear()
        for (index in TEXTURE_LABELS.indices) {
            val row = inflater.inflate(R.layout.list_item_primedgun_cannon_texture, b.cannonTextureRows, false)
            row.findViewById<TextView>(R.id.cannon_texture_label).setText(TEXTURE_LABELS[index])
            val import = row.findViewById<Button>(R.id.cannon_texture_import)
            import.setOnClickListener { onImportClicked(index) }
            b.cannonTextureRows.addView(row)
            rows.add(
                TextureRow(
                    row.findViewById(R.id.cannon_texture_preview),
                    row.findViewById(R.id.cannon_texture_no_preview),
                    row.findViewById(R.id.cannon_texture_path),
                    import
                )
            )
        }

        b.cannonApply.setOnClickListener { applySlot() }
        b.cannonRemoveShine.setOnClickListener { removeShine() }
        b.cannonRestoreShine.setOnClickListener { restoreShine() }
        b.cannonSlotFolder.setOnClickListener {
            showMessage(getString(R.string.primedgun_cannon_slot_folder_message, LauncherNative.cannonLibraryFolder()))
        }
        b.cannonActivePack.setOnClickListener {
            showMessage(
                getString(R.string.primedgun_cannon_active_pack_message, LauncherNative.cannonPackFolders().joinToString("\n"))
            )
        }
        refresh()
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
        rows.clear()
        radios.clear()
        appliedSlot = -1
    }

    /**
     * Follows the applied slot when it changed (Reset All, a transfer, the game's own
     * rewrite); otherwise keeps the player's selection, which Apply has not used yet.
     */
    override fun refresh() {
        val b = binding ?: return
        val applied = LauncherSettings.int(SLOT_KEY).coerceIn(0, SLOT_COUNT - 1)
        if (applied != appliedSlot) {
            appliedSlot = applied
            selectedSlot = applied
            setStatus(
                if (applied == SLOT_DEFAULT) getString(R.string.primedgun_cannon_default_active)
                else getString(R.string.primedgun_cannon_slot_active, slotName(applied))
            )
        }
        radios.firstOrNull { it.tag == selectedSlot }?.isChecked = true
        refreshRows()
        // Applying writes the pack and the setting, which the game owns while it runs.
        val locked = launcher?.gameRunning ?: false
        radios.forEach { it.isEnabled = !locked }
        for (button in listOf(b.cannonApply, b.cannonRemoveShine, b.cannonRestoreShine)) {
            button.isEnabled = !locked
        }
        rows.forEach { it.import.isEnabled = !locked }
    }

    private fun refreshRows() {
        val custom = selectedSlot == SLOT_CUSTOM
        for ((index, row) in rows.withIndex()) {
            row.import.isVisible = custom
            val source = LauncherNative.cannonSource(selectedSlot, index)
            if (selectedSlot == SLOT_DEFAULT) {
                row.path.setText(R.string.primedgun_cannon_default_no_override)
            } else {
                row.path.text = source.ifEmpty { getString(R.string.primedgun_cannon_no_texture_imported) }
            }
            setPreview(row, source)
        }
    }

    private fun setPreview(row: TextureRow, path: String) {
        val bitmap = loadPreview(path)
        row.preview.setImageBitmap(bitmap)
        row.noPreview.isVisible = bitmap == null
    }

    // PNG through Android, DXT1 .dds through the PC launcher's decoder, both at
    // thumbnail size: a file the player imported can be any size, and a preview that
    // cannot be made is only a missing preview.
    private fun loadPreview(path: String): Bitmap? = runCatching {
        if (path.isEmpty() || !File(path).isFile) return@runCatching null
        decodePngThumbnail(path) ?: LauncherNative.decodeDds(path)?.let { decoded ->
            val width = decoded[0]
            val height = decoded[1]
            Bitmap.createBitmap(decoded, 2, width, width, height, Bitmap.Config.ARGB_8888)
        }
    }.getOrNull()

    private fun decodePngThumbnail(path: String): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(path, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        var sample = 1
        while (bounds.outWidth / sample > PREVIEW_SIDE || bounds.outHeight / sample > PREVIEW_SIDE) {
            sample *= 2
        }
        return BitmapFactory.decodeFile(path, BitmapFactory.Options().apply { inSampleSize = sample })
    }

    // Copies the slot into the pack and records it as the active one.
    private fun applyAndRecord(slot: Int): Boolean {
        val error = LauncherNative.cannonApply(slot)
        if (error.isNotEmpty()) {
            showMessage(getString(R.string.primedgun_cannon_apply_failed, slotName(slot), error))
            return false
        }
        LauncherSettings.setInt(SLOT_KEY, slot)
        appliedSlot = slot
        LauncherSettings.saveKey(SLOT_KEY)?.let { showMessage(getString(R.string.primedgun_settings_save_failed, it)) }
        launcher?.onSettingsEdited()
        return true
    }

    private fun applySlot() {
        val slot = selectedSlot
        if (!applyAndRecord(slot)) return
        refreshRows()
        setStatus(
            if (slot == SLOT_DEFAULT) getString(R.string.primedgun_cannon_default_applied)
            else getString(R.string.primedgun_cannon_slot_applied, slotName(slot))
        )
    }

    private fun onImportClicked(index: Int) {
        if (selectedSlot != SLOT_CUSTOM) {
            showMessage(getString(R.string.primedgun_cannon_choose_custom))
            return
        }
        importIndex = index
        importSlot = selectedSlot
        requestTexture.launch(arrayOf("*/*"))
    }

    private fun importTexture(uri: Uri) {
        val index = importIndex
        importIndex = -1
        if (index < 0) return
        val context = requireContext()
        val name = DiscCopy.displayName(context, uri) ?: ""
        val extension = name.substringAfterLast('.', "").lowercase()
        if (extension != "png" && extension != "dds") {
            showMessage(getString(R.string.primedgun_cannon_png_or_dds))
            return
        }
        // The core imports from a path; the picked document is staged with its extension.
        val staged = File(context.cacheDir, "cannon_import.$extension")
        try {
            val input = context.contentResolver.openInputStream(uri) ?: throw IOException(name)
            input.use { source -> staged.outputStream().use { source.copyTo(it) } }
            val error = LauncherNative.cannonImport(importSlot, index, staged.path)
            if (error.isNotEmpty()) {
                showMessage(error)
                return
            }
        } catch (e: IOException) {
            showMessage(getString(R.string.primedgun_cannon_copy_failed))
            return
        } finally {
            staged.delete()
        }
        refreshRows()
        setStatus(getString(R.string.primedgun_cannon_imported, slotName(importSlot)))
    }

    private fun removeShine() {
        val slot = selectedSlot
        if (slot <= SLOT_DEFAULT) {
            showMessage(getString(R.string.primedgun_cannon_choose_slot_remove_shine))
            return
        }
        val error = LauncherNative.cannonRemoveShine(slot)
        if (error.isNotEmpty()) {
            showMessage(error)
            return
        }
        if (!applyAndRecord(slot)) return
        refreshRows()
        setStatus(getString(R.string.primedgun_cannon_remove_shine_applied, slotName(slot)))
    }

    private fun restoreShine() {
        val slot = selectedSlot
        if (slot <= SLOT_DEFAULT) {
            showMessage(getString(R.string.primedgun_cannon_choose_slot_restore_shine))
            return
        }
        val error = LauncherNative.cannonRestoreShine(slot)
        if (error.isNotEmpty()) {
            showMessage(error)
            return
        }
        if (!applyAndRecord(slot)) return
        refreshRows()
        setStatus(getString(R.string.primedgun_cannon_restore_shine_applied, slotName(slot)))
    }

    private fun slotName(slot: Int): String = when (slot) {
        SLOT_DEFAULT -> getString(R.string.primedgun_cannon_slot_default)
        SLOT_CUSTOM -> getString(R.string.primedgun_cannon_slot_custom)
        else -> getString(R.string.primedgun_cannon_slot_n, slot)
    }

    private fun setStatus(text: String) {
        binding?.cannonStatus?.text = text
    }

    private fun showMessage(message: String) {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.primedgun_tab_cannon_textures)
            .setMessage(message)
            .setPositiveButton(R.string.primedgun_ok, null)
            .show()
    }
}
