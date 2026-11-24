// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

package org.stratoemu.strato.applet.swkbd

import android.view.Window
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

/** Applies the public immersive request to the keyboard's own focused window, without hiding IME. */
object SoftwareKeyboardWindow {
    fun hideNavigationBar(window : Window) {
        WindowInsetsControllerCompat(window, window.decorView).apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            // Android 12 WindowManager may force this bar visible while IME is shown.
            // This request is deferred by the OS when the app does not control navigation bars.
            hide(WindowInsetsCompat.Type.navigationBars())
        }
    }
}
