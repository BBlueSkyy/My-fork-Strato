// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

package org.stratoemu.strato.applet.swkbd

import android.view.View

/** Temporarily suppresses the touch controls without changing the user's visibility settings. */
class SoftwareKeyboardControls(private val views : List<View>, private val releaseTouches : () -> Unit) {
    private var savedVisibility : List<Int>? = null

    /** Keyboard and picture-in-picture suppression share one snapshot, including across replacements. */
    fun update(keyboardActive : Boolean, pictureInPicture : Boolean) {
        if (keyboardActive || pictureInPicture) {
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
