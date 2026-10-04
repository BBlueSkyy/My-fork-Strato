/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.emulation

import android.graphics.BitmapFactory
import org.stratoemu.strato.R
import org.stratoemu.strato.data.BaseAppItem
import org.stratoemu.strato.databinding.ProgramRelaunchLoadingBinding

object ProgramRelaunchUi {
    fun configure(binding : ProgramRelaunchLoadingBinding, item : BaseAppItem?, snapshotPath : String?) {
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
        when {
            snapshot != null -> binding.backgroundImage.setImageBitmap(snapshot)
            item != null -> binding.backgroundImage.setImageBitmap(item.bitmapIcon)
            else -> binding.backgroundImage.setImageResource(R.drawable.default_icon)
        }

        binding.progressBar.isIndeterminate = true
    }
}
