// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from PrimedGun's Quest launcher (features/primedgun/ui/PrimedGunAboutFragment.kt,
// Copyright 2026 PrimedGun Project, GPL-2.0-or-later).
package org.primedgun.v2.launcher

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.fragment.app.Fragment
import org.primedgun.v2.BuildConfig
import org.primedgun.v2.QuestStorage
import org.primedgun.v2.R
import org.primedgun.v2.databinding.FragmentPrimedgunAboutBinding

/**
 * The About tab: version and build, tips for playing on Quest, links and credits, with
 * the native port's own lineage added (launcher/ui/about_tab.cpp on the PC).
 */
class PrimedGunAboutFragment : Fragment() {

    companion object {
        private const val GITHUB_URL = "https://github.com/Nobbie248/PrimedGun"
        private const val DISCORD_URL = "https://discord.gg/GdmffzCTrh"
    }

    private var binding: FragmentPrimedgunAboutBinding? = null

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?): View {
        val inflated = FragmentPrimedgunAboutBinding.inflate(inflater, container, false)
        binding = inflated
        return inflated.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        val b = binding ?: return
        b.aboutVersion.text = getString(R.string.primedgun_about_version, "v" + BuildConfig.VERSION_NAME)
        b.aboutBuild.text = getString(R.string.primedgun_about_build, BuildConfig.BUILD_TYPE, LauncherNative.buildRevision())
        b.aboutUserFolder.text = getString(R.string.primedgun_about_user_folder, QuestStorage.userFolder(requireContext()).path)
        b.aboutGithub.setOnClickListener { openLink(GITHUB_URL) }
        b.aboutDiscord.setOnClickListener { openLink(DISCORD_URL) }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        binding = null
    }

    private fun openLink(url: String) {
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (e: ActivityNotFoundException) {
            Toast.makeText(requireContext(), getString(R.string.primedgun_about_no_browser, url), Toast.LENGTH_LONG).show()
        }
    }
}
