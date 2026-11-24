// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

package org.stratoemu.strato.applet.swkbd

import android.content.Context
import android.util.AttributeSet
import android.view.KeyEvent
import com.google.android.material.textfield.TextInputEditText

/** Reports an actual Back key before the IME consumes it; insets never end an applet session. */
class SoftwareKeyboardEditText @JvmOverloads constructor(context : Context, attrs : AttributeSet? = null) : TextInputEditText(context, attrs) {
    var onImeBackPressed : (() -> Unit)? = null

    override fun onKeyPreIme(keyCode : Int, event : KeyEvent) : Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && onImeBackPressed != null) {
            if (event.action == KeyEvent.ACTION_DOWN) {
                if (event.repeatCount == 0)
                    keyDispatcherState?.startTracking(event, this)
                return true
            }
            if (event.action == KeyEvent.ACTION_UP) {
                keyDispatcherState?.handleUpEvent(event)
                if (event.isTracking && !event.isCanceled)
                    onImeBackPressed?.invoke()
                return true
            }
        }
        return super.onKeyPreIme(keyCode, event)
    }
}
