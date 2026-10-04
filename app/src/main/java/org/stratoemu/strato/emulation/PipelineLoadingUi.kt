/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.emulation

import android.graphics.Bitmap
import android.graphics.RenderEffect
import android.graphics.Shader
import android.os.Build
import androidx.core.view.isGone
import com.google.android.renderscript.Toolkit
import org.stratoemu.strato.R
import org.stratoemu.strato.data.BaseAppItem
import org.stratoemu.strato.databinding.PipelineLoadingBinding

/**
 * Reuses Strato's cached-pipeline loading UI for short frontend-only transitions.
 * This never controls emulation lifecycle; it only configures the existing layout.
 */
object PipelineLoadingUi {
    fun configureIndeterminate(
        binding : PipelineLoadingBinding,
        item : BaseAppItem?,
        fallbackBackground : Bitmap? = null
    ) {
        if (item != null) {
            binding.gameTitle.apply {
                text = item.title
                isSelected = true
            }
            binding.gameVersion.text = item.version
            binding.gameIcon.setImageBitmap(item.bitmapIcon)

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                binding.backgroundImage.setImageBitmap(item.bitmapIcon)
                binding.backgroundImage.setRenderEffect(
                    RenderEffect.createBlurEffect(75f, 75f, Shader.TileMode.MIRROR)
                )
            } else {
                binding.backgroundImage.setImageBitmap(Toolkit.blur(item.bitmapIcon, 15))
            }
        } else {
            binding.gameTitle.text = ""
            binding.gameVersion.text = ""
            binding.gameIcon.setImageResource(R.drawable.default_icon)
            if (fallbackBackground != null)
                binding.backgroundImage.setImageBitmap(fallbackBackground)
            else
                binding.backgroundImage.setImageResource(R.drawable.default_icon)
        }

        binding.progressBar.isIndeterminate = true
        binding.progressLabel.isGone = true
    }
}
