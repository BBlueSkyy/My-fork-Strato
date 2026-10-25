/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.settings

import android.content.Context
import org.stratoemu.strato.utils.GpuDriverHelper
import kotlinx.serialization.Serializable

/**
 * The settings that will be passed to libskyline when running and executable
 */
@Serializable
@Suppress("unused")
data class NativeSettings(
    // System
    var isDocked : Boolean,
    var usernameValue : String,
    var profilePictureValue : String,
    var systemLanguage : Int,
    var systemRegion : Int,
    var isInternetEnabled : Boolean,

    // Audio
    var isAudioOutputDisabled : Boolean,

    // GPU
    var gpuDriver : String,
    var gpuDriverLibraryName : String,
    var forceTripleBuffering : Boolean,
    var disableFrameThrottling : Boolean,
    var balancedFullscreenCurve : Float,
    var executorSlotCountScale : Int,
    var executorFlushThreshold : Int,
    var useDirectMemoryImport : Boolean,
    var forceMaxGpuClocks : Boolean,
    var freeGuestTextureMemory : Boolean,
    var disableShaderCache : Boolean,

    // Hacks
    var enableFastGpuReadbackHack : Boolean,
    var enableFastReadbackWrites : Boolean,
    var disableSubgroupShuffle : Boolean,

    // Debug
    var logLevel : Int
) {
    constructor(context : Context, pref : EmulationSettings) : this(
        pref.isDocked,
        pref.usernameValue,
        pref.profilePictureValue,
        pref.systemLanguage,
        pref.systemRegion,
        pref.isInternetEnabled,
        pref.isAudioOutputDisabled,
        if (pref.gpuDriver == EmulationSettings.SYSTEM_GPU_DRIVER) "" else pref.gpuDriver,
        if (pref.gpuDriver == EmulationSettings.SYSTEM_GPU_DRIVER) "" else GpuDriverHelper.getLibraryName(context, pref.gpuDriver),
        pref.forceTripleBuffering,
        pref.disableFrameThrottling,
        calculateBalancedFullscreenCurve(context, pref.aspectRatio),
        pref.executorSlotCountScale,
        pref.executorFlushThreshold,
        pref.useDirectMemoryImport,
        pref.forceMaxGpuClocks,
        pref.freeGuestTextureMemory,
        pref.disableShaderCache,
        pref.enableFastGpuReadbackHack,
        pref.enableFastReadbackWrites,
        pref.disableSubgroupShuffle,
        pref.logLevel
    )

    /**
     * Updates settings in libskyline during emulation
     */
    external fun updateNative()
}

// Center-weighted presentation keeps the center close to 16:9 while the
// periphery absorbs the extra width. A full 16:9 image remains visible.
private fun calculateBalancedFullscreenCurve(context : Context, aspectRatioMode : Int) : Float {
    if (aspectRatioMode != 3)
        return 0f

    val metrics = context.resources.displayMetrics
    val width = maxOf(metrics.widthPixels, metrics.heightPixels)
    val height = minOf(metrics.widthPixels, metrics.heightPixels)
    if (height <= 0)
        return 0f

    val horizontalStretch = (width.toFloat() / height) / (16f / 9f)
    if (horizontalStretch <= 1f)
        return 0f

    // In the center: displayed horizontal scale = stretch * (1 - curve / 4).
    // Aim for only ~4% center distortion while keeping the mapping monotonic.
    return (4f * (1f - 1.04f / horizontalStretch)).coerceIn(0f, 0.9f)
}
