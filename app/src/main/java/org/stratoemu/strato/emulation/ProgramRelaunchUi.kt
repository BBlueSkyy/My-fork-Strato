/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.emulation

import android.graphics.BitmapFactory
import android.view.View
import org.stratoemu.strato.R
import org.stratoemu.strato.data.BaseAppItem
import org.stratoemu.strato.databinding.ProgramRelaunchLoadingBinding

/**
 * Configures the lightweight relaunch overlay used while switching application programs.
 * When [snapshotPath] is null the background stays transparent so the old game frame remains
 * visible underneath; cross-process stages use the persisted last game frame instead.
 */
object ProgramRelaunchUi {
    fun configure(
        binding : ProgramRelaunchLoadingBinding,
        item : BaseAppItem?,
        snapshotPath : String?,
        transparentBackground : Boolean
    ) {
        if (item != null) {
            binding.gameTitle.text = item.title
            binding.gameVersion.text = item.version
            binding.gameIcon.setImageBitmap(item.bitmapIcon)
        } else {
            binding.gameTitle.text = ""
            binding.gameVersion.text = ""
            binding.gameIcon.setImageResource(R.drawable.default_icon)
        }

        val snapshot = snapshotPath?.let(BitmapFactory::decodeFile)
        if (snapshot != null) {
            binding.backgroundImage.setImageBitmap(snapshot)
            binding.backgroundImage.visibility = View.VISIBLE
        } else if (transparentBackground) {
            binding.backgroundImage.setImageDrawable(null)
            binding.backgroundImage.visibility = View.INVISIBLE
        } else if (item != null) {
            binding.backgroundImage.setImageBitmap(item.bitmapIcon)
            binding.backgroundImage.visibility = View.VISIBLE
        } else {
            binding.backgroundImage.setImageResource(R.drawable.default_icon)
            binding.backgroundImage.visibility = View.VISIBLE
        }

        binding.progressBar.isIndeterminate = true
    }
}
