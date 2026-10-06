// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from PrimedGun's Quest launcher (features/primedgun/ui/PrimedGunSettingsFragment.kt,
// Copyright 2026 PrimedGun Project, GPL-2.0-or-later).
package org.primedgun.v2.launcher

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.primedgun.v2.R

/** A launcher tab made of [PrimedGunItem] rows: Controller, Calibration and Port Config. */
class PrimedGunSettingsFragment : Fragment(), PrimedGunRefreshable {

    companion object {
        private const val ARG_TAB = "tab"

        fun newInstance(tab: PrimedGunTabs.Tab): PrimedGunSettingsFragment =
            PrimedGunSettingsFragment().apply {
                arguments = Bundle().apply { putString(ARG_TAB, tab.name) }
            }
    }

    private var adapter: PrimedGunItemAdapter? = null

    private val tab: PrimedGunTabs.Tab
        get() = PrimedGunTabs.Tab.valueOf(
            requireArguments().getString(ARG_TAB) ?: PrimedGunTabs.Tab.CONTROLLER.name
        )

    private val launcher: LauncherActivity? get() = activity as? LauncherActivity

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View = inflater.inflate(R.layout.fragment_primedgun_settings, container, false)

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val list = view.findViewById<RecyclerView>(R.id.primedgun_settings_list)
        val itemAdapter = PrimedGunItemAdapter(PrimedGunTabs.itemsFor(requireContext(), tab)) {
            launcher?.onSettingsEdited()
        }
        adapter = itemAdapter
        list.layoutManager = LinearLayoutManager(requireContext())
        list.adapter = itemAdapter
        refresh()
    }

    override fun onResume() {
        super.onResume()
        // The game rewrites the settings file when it exits, so re-read on return rather
        // than trusting what was on screen when this tab lost focus.
        refresh()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        adapter = null
    }

    override fun refresh() {
        val current = adapter ?: return
        current.locked = launcher?.gameRunning ?: false
        current.refresh()
    }
}
