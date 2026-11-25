// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

package org.stratoemu.strato.input.onscreen

import android.view.View

/** Preserves user control visibility while picture-in-picture temporarily hides the overlay. */
class OnScreenControlsVisibility(private val views : List<View>, private val releaseTouches : () -> Unit) {
    private var savedVisibility : List<Int>? = null

    /** This is a PiP-only operation; opening a software keyboard does not call it. */
    fun update(pictureInPicture : Boolean) {
        if (pictureInPicture) {
            if (savedVisibility == null) {
                savedVisibility = views.map { it.visibility }
                releaseTouches()
            }
            views.forEach { it.visibility = View.GONE }
        } else {
            savedVisibility?.let { visibility ->
                views.zip(visibility).forEach { (view, value) -> view.visibility = value }
            }
            savedVisibility = null
        }
    }
}
