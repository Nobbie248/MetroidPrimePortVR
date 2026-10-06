// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from PrimedGun's Quest launcher (features/primedgun/ui/PrimedGunItemAdapter.kt,
// Copyright 2026 PrimedGun Project, GPL-2.0-or-later).
package org.primedgun.v2.launcher

import android.annotation.SuppressLint
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.slider.Slider
import java.util.Locale
import kotlin.math.roundToInt
import org.primedgun.v2.R

/**
 * Renders [PrimedGunItem] rows. Every edit goes to the launcher's working copy of
 * port_settings.ini at once and [onEdited] tells the window, which shows that there
 * are unsaved changes; Save Settings or Play writes them, as in the Qt launcher.
 *
 * While the game runs the rows are [locked]: the game rewrites the whole file when it
 * exits, so edits made meanwhile would be lost.
 */
class PrimedGunItemAdapter(
    private val items: List<PrimedGunItem>,
    private val onEdited: () -> Unit,
) : RecyclerView.Adapter<PrimedGunItemAdapter.ViewHolder>() {

    companion object {
        private const val TYPE_HEADER = 0
        private const val TYPE_NOTE = 1
        private const val TYPE_ACTION = 2
        private const val TYPE_SWITCH = 3
        private const val TYPE_SLIDER = 4
        private const val TYPE_CHOICE = 5
        private const val TYPE_TOGGLE2 = 6
    }

    class ViewHolder(view: View) : RecyclerView.ViewHolder(view)

    var locked = false
        @SuppressLint("NotifyDataSetChanged")
        set(value) {
            if (field != value) {
                field = value
                notifyDataSetChanged()
            }
        }

    override fun getItemCount(): Int = items.size

    override fun getItemViewType(position: Int): Int = when (items[position]) {
        is PrimedGunItem.Header -> TYPE_HEADER
        is PrimedGunItem.Note -> TYPE_NOTE
        is PrimedGunItem.Action -> TYPE_ACTION
        is PrimedGunItem.Switch -> TYPE_SWITCH
        is PrimedGunItem.Slider -> TYPE_SLIDER
        is PrimedGunItem.Choice -> TYPE_CHOICE
        is PrimedGunItem.Toggle2 -> TYPE_TOGGLE2
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val layout = when (viewType) {
            TYPE_HEADER -> R.layout.list_item_primedgun_header
            TYPE_NOTE -> R.layout.list_item_primedgun_note
            TYPE_ACTION -> R.layout.list_item_primedgun_action
            TYPE_SWITCH -> R.layout.list_item_primedgun_switch
            TYPE_SLIDER -> R.layout.list_item_primedgun_slider
            TYPE_CHOICE -> R.layout.list_item_primedgun_choice
            TYPE_TOGGLE2 -> R.layout.list_item_primedgun_toggle2
            else -> throw IllegalArgumentException("Unknown PrimedGun item type $viewType")
        }
        return ViewHolder(LayoutInflater.from(parent.context).inflate(layout, parent, false))
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        when (val item = items[position]) {
            is PrimedGunItem.Header -> bindHeader(holder, item)
            is PrimedGunItem.Note -> bindNote(holder, item)
            is PrimedGunItem.Action -> bindAction(holder, item)
            is PrimedGunItem.Switch -> bindSwitch(holder, item)
            is PrimedGunItem.Slider -> bindSlider(holder, item)
            is PrimedGunItem.Choice -> bindChoice(holder, item)
            is PrimedGunItem.Toggle2 -> bindToggle2(holder, item)
        }
    }

    /** Re-reads every row from the settings. Used after an action rewrote many settings. */
    @SuppressLint("NotifyDataSetChanged")
    fun refresh() = notifyDataSetChanged()

    private fun bindHeader(holder: ViewHolder, item: PrimedGunItem.Header) {
        holder.itemView.findViewById<TextView>(R.id.primedgun_header_title).text = item.title
    }

    private fun bindNote(holder: ViewHolder, item: PrimedGunItem.Note) {
        holder.itemView.findViewById<TextView>(R.id.primedgun_note_text).text = item.text
    }

    private fun bindAction(holder: ViewHolder, item: PrimedGunItem.Action) {
        val button = holder.itemView.findViewById<Button>(R.id.primedgun_action_button)
        button.text = item.title
        button.isEnabled = !locked || !item.edits
        button.setOnClickListener {
            item.onClick()
            if (item.edits) {
                // Action buttons rewrite whole sections, so every visible row may be stale now.
                refresh()
                onEdited()
            }
        }
    }

    // The "not active yet" tag: the game saves this setting but does not use it yet.
    private fun bindTag(holder: ViewHolder, id: Int, inactive: Boolean) {
        holder.itemView.findViewById<TextView>(id).visibility = if (inactive) View.VISIBLE else View.GONE
    }

    private fun bindDescription(view: TextView, text: String?) {
        if (text.isNullOrEmpty()) {
            view.visibility = View.GONE
        } else {
            view.visibility = View.VISIBLE
            view.text = text
        }
    }

    private fun bindSwitch(holder: ViewHolder, item: PrimedGunItem.Switch) {
        val title = holder.itemView.findViewById<TextView>(R.id.primedgun_switch_title)
        val description = holder.itemView.findViewById<TextView>(R.id.primedgun_switch_description)
        val switch = holder.itemView.findViewById<MaterialSwitch>(R.id.primedgun_switch)

        title.text = item.title
        bindDescription(description, item.description)
        bindTag(holder, R.id.primedgun_switch_tag, item.inactive)

        // Clear before assigning: a recycled row would otherwise write the previous row's value.
        switch.setOnCheckedChangeListener(null)
        switch.isChecked = item.get()
        switch.isEnabled = !locked
        switch.setOnCheckedChangeListener { _, checked ->
            item.set(checked)
            onEdited()
            if (item.refreshes) {
                holder.itemView.post { refresh() }
            }
        }
        holder.itemView.isEnabled = !locked
        holder.itemView.setOnClickListener {
            if (!locked) {
                switch.isChecked = !switch.isChecked
            }
        }
    }

    private fun bindSlider(holder: ViewHolder, item: PrimedGunItem.Slider) {
        val title = holder.itemView.findViewById<TextView>(R.id.primedgun_slider_title)
        val description = holder.itemView.findViewById<TextView>(R.id.primedgun_slider_description)
        val value = holder.itemView.findViewById<TextView>(R.id.primedgun_slider_value)
        val slider = holder.itemView.findViewById<Slider>(R.id.primedgun_slider)
        val minus = holder.itemView.findViewById<Button>(R.id.primedgun_slider_minus)
        val plus = holder.itemView.findViewById<Button>(R.id.primedgun_slider_plus)

        title.text = item.title
        bindDescription(description, item.description)
        bindTag(holder, R.id.primedgun_slider_tag, item.inactive)

        // The slider runs in whole step units, the way MainWindow scales its QSlider. Handing
        // Material a fractional stepSize risks its "not divisible" validation on float rounding.
        val steps = ((item.max - item.min) / item.step).roundToInt()
        val toUnits = { v: Float -> ((v - item.min) / item.step).roundToInt().coerceIn(0, steps) }
        val toValue = { units: Float -> item.min + units * item.step }
        val show = { v: Float -> item.format?.invoke(v) ?: format(v, item.decimals) }

        slider.clearOnChangeListeners()
        slider.valueFrom = 0f
        slider.valueTo = steps.toFloat()
        slider.stepSize = 1f
        slider.value = toUnits(item.get()).toFloat()
        value.text = show(toValue(slider.value))

        // Only the player's moves write: a rebind sets the value too.
        val commit = { units: Float ->
            item.set(toValue(units))
            onEdited()
            if (item.refreshes) {
                holder.itemView.post { refresh() }
            }
        }
        slider.addOnChangeListener { _, units, fromUser ->
            value.text = show(toValue(units))
            if (fromUser) {
                commit(units)
            }
        }
        val editable = !locked && item.enabled()
        minus.setOnClickListener {
            if (!editable) return@setOnClickListener
            slider.value = (slider.value - 1f).coerceAtLeast(0f)
            commit(slider.value)
        }
        plus.setOnClickListener {
            if (!editable) return@setOnClickListener
            slider.value = (slider.value + 1f).coerceAtMost(steps.toFloat())
            commit(slider.value)
        }
        slider.isEnabled = editable
        minus.isEnabled = editable
        plus.isEnabled = editable
    }

    private fun bindChoice(holder: ViewHolder, item: PrimedGunItem.Choice) {
        val title = holder.itemView.findViewById<TextView>(R.id.primedgun_choice_title)
        val description = holder.itemView.findViewById<TextView>(R.id.primedgun_choice_description)
        val value = holder.itemView.findViewById<TextView>(R.id.primedgun_choice_value)

        title.text = item.title
        bindDescription(description, item.description)
        bindTag(holder, R.id.primedgun_choice_tag, item.inactive)
        // Read on every use rather than caching: the dialog can be reopened without a rebind.
        val selectedIndex = { item.values.indexOf(item.get()).takeIf { it >= 0 } ?: 0 }
        value.text = item.labels[selectedIndex()]
        value.isEnabled = !locked

        val showDialog = View.OnClickListener { view ->
            if (locked) {
                return@OnClickListener
            }
            MaterialAlertDialogBuilder(view.context)
                .setTitle(item.title)
                .setSingleChoiceItems(item.labels.toTypedArray(), selectedIndex()) { dialog, which ->
                    item.set(item.values[which])
                    value.text = item.labels[which]
                    onEdited()
                    dialog.dismiss()
                }
                .show()
        }
        holder.itemView.isEnabled = !locked
        holder.itemView.setOnClickListener(showDialog)
        value.setOnClickListener(showDialog)
    }

    private fun bindToggle2(holder: ViewHolder, item: PrimedGunItem.Toggle2) {
        val title = holder.itemView.findViewById<TextView>(R.id.primedgun_toggle2_title)
        val group = holder.itemView.findViewById<RadioGroup>(R.id.primedgun_toggle2_group)
        val optionA = holder.itemView.findViewById<RadioButton>(R.id.primedgun_toggle2_a)
        val optionB = holder.itemView.findViewById<RadioButton>(R.id.primedgun_toggle2_b)

        if (item.title.isNullOrEmpty()) {
            title.visibility = View.GONE
        } else {
            title.visibility = View.VISIBLE
            title.text = item.title
        }
        bindTag(holder, R.id.primedgun_toggle2_tag, item.inactive)
        optionA.text = item.labelA
        optionB.text = item.labelB

        group.setOnCheckedChangeListener(null)
        group.check(if (item.get()) optionB.id else optionA.id)
        optionA.isEnabled = !locked
        optionB.isEnabled = !locked
        group.setOnCheckedChangeListener { _, checkedId ->
            item.set(checkedId == optionB.id)
            onEdited()
        }
    }

    private fun format(value: Float, decimals: Int): String =
        String.format(Locale.getDefault(), "%.${decimals}f", value)
}
