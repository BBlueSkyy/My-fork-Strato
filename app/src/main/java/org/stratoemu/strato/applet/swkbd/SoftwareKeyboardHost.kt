// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

package org.stratoemu.strato.applet.swkbd

import androidx.fragment.app.FragmentActivity
import org.stratoemu.strato.utils.ByteBufferSerializable
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** The Activity forwards dialog events to the same host that owns its displayed controls. */
interface SoftwareKeyboardHostCallbacks {
    fun sendSoftwareKeyboardEvent(sessionId : Long, dialog : SoftwareKeyboardDialog, type : Int, text : String, cursor : Int)
    fun onSoftwareKeyboardShown(sessionId : Long, dialog : SoftwareKeyboardDialog)
    fun onSoftwareKeyboardDismissed(sessionId : Long, dialog : SoftwareKeyboardDialog)
}

/** Owns SWKBD UI sessions on the main thread, independently of the native emulation engine. */
class SoftwareKeyboardHost(
    private val activity : FragmentActivity,
    private val controls : SoftwareKeyboardControls,
    private val nativeEvent : (Long, Int, String, Int) -> Unit
) {
    private val dialogs = mutableMapOf<Long, SoftwareKeyboardDialog>()
    val isActive get() = dialogs.isNotEmpty()

    fun updateControls() {
        controls.update(isActive, activity.isInPictureInPictureMode)
    }

    fun destroy() {
        dialogs.toMap().also { dialogs.clear() }.forEach { (sessionId, dialog) ->
            dialog.closeFromFrontend()
            nativeEvent(sessionId, SoftwareKeyboardDialog.eventFrontendDestroyed, "", 0)
        }
    }

    fun open(sessionId : Long, buffer : ByteBuffer, initialText : String, inline : Boolean) : Boolean {
        if (activity.isFinishing || activity.isDestroyed) {
            return false
        }
        val config = try {
            buffer.order(ByteOrder.LITTLE_ENDIAN)
            ByteBufferSerializable.createFromByteBuffer(SoftwareKeyboardConfig::class, buffer) as SoftwareKeyboardConfig
        } catch (exception : Exception) {
            return false
        }

        activity.runOnUiThread {
            if (activity.isFinishing || activity.isDestroyed) {
                nativeEvent(sessionId, SoftwareKeyboardDialog.eventFrontendDestroyed, "", 0)
                return@runOnUiThread
            }

            dialogs.toMap().forEach { (otherSessionId, otherDialog) ->
                if (otherSessionId != sessionId) {
                    dialogs.remove(otherSessionId)
                    otherDialog.closeFromFrontend()
                    nativeEvent(otherSessionId, SoftwareKeyboardDialog.eventFrontendDestroyed, "", 0)
                }
            }

            dialogs.remove(sessionId)?.closeFromFrontend()
            val dialog = SoftwareKeyboardDialog.newInstance(sessionId, config, initialText, inline)
            dialogs[sessionId] = dialog
            updateControls()
            try {
                dialog.showNow(activity.supportFragmentManager, "software-keyboard-$sessionId")
                nativeEvent(sessionId, SoftwareKeyboardDialog.eventFrontendOpened, "", 0)
            } catch (exception : IllegalStateException) {
                dialogs.remove(sessionId)
                dialog.closeFromFrontend()
                nativeEvent(sessionId, SoftwareKeyboardDialog.eventFrontendDestroyed, "", 0)
            } finally {
                updateControls()
            }
        }
        return true
    }

    fun close(sessionId : Long) {
        activity.runOnUiThread {
            // Keep the gate until the actual dialog/window reports dismissal.
            dialogs[sessionId]?.closeFromFrontend()
        }
    }

    fun hide(sessionId : Long) = close(sessionId)

    fun update(sessionId : Long, text : String, cursor : Int) {
        activity.runOnUiThread { dialogs[sessionId]?.updateFromFrontend(text, cursor) }
    }

    fun resume(sessionId : Long) {
        activity.runOnUiThread { dialogs[sessionId]?.resumeEditing() }
    }

    fun showTextCheck(sessionId : Long, result : Int, message : String) {
        activity.runOnUiThread { dialogs[sessionId]?.showTextCheck(result, message) }
    }

    fun sendEvent(sessionId : Long, dialog : SoftwareKeyboardDialog, type : Int, text : String, cursor : Int) {
        if (dialogs[sessionId] === dialog)
            nativeEvent(sessionId, type, text, cursor)
    }

    /** FragmentManager can show a restored dialog without reentering the JNI open method. */
    fun onShown(sessionId : Long, dialog : SoftwareKeyboardDialog) {
        val current = dialogs[sessionId]
        if (current !== dialog && dialogs.isNotEmpty()) {
            dialog.closeFromFrontend()
            return
        }
        dialogs[sessionId] = dialog
        updateControls()
    }

    fun onDismissed(sessionId : Long, dialog : SoftwareKeyboardDialog) {
        if (dialogs[sessionId] === dialog) {
            dialogs.remove(sessionId)
            updateControls()
        }
    }
}
