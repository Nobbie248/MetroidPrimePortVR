// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from PrimedGun's Quest launcher (features/primedgun/ui/PrimedGunLayoutFragment.kt,
// Copyright 2026 PrimedGun Project, GPL-2.0-or-later).
package org.primedgun.v2.launcher

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import org.primedgun.v2.R

/**
 * The Layout tab: the controller binding diagram. The port's PrimedGun control scheme
 * (platform/vr/vr_pad.cpp) uses PrimedGun's bindings.
 */
class PrimedGunLayoutFragment : Fragment() {
    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View = inflater.inflate(R.layout.fragment_primedgun_layout, container, false)
}
