// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

package org.stratoemu.strato.applet.swkbd

import androidx.fragment.app.FragmentActivity
import org.stratoemu.strato.utils.ByteBufferSerializable
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** The Activity forwards dialog events to the host that owns the SWKBD session. */
interface SoftwareKeyboardHostCallbacks {
    fun sendSoftwareKeyboardEvent(sessionId : Long, dialog : SoftwareKeyboardDialog, type : Int, text : String, cursor : Int)
    fun onSoftwareKeyboardShown(sessionId : Long, dialog : SoftwareKeyboardDialog)
    fun onSoftwareKeyboardDismissed(sessionId : Long, dialog : SoftwareKeyboardDialog)
    fun traceSoftwareKeyboard(sessionId : Long, dialog : SoftwareKeyboardDialog, stage : String, detail : Int) {}
}

/** Owns SWKBD UI sessions on the main thread, independently of the native emulation engine. */
class SoftwareKeyboardHost(
    private val activity : FragmentActivity,
    private val diagnostic : (Long, String, Int) -> Unit = { _, _, _ -> },
    private val nativeEvent : (Long, Int, String, Int) -> Unit
) {
    private val dialogs = mutableMapOf<Long, SoftwareKeyboardDialog>()
    val isActive get() = dialogs.isNotEmpty()

    fun destroy() {
        dialogs.toMap().also { dialogs.clear() }.forEach { (sessionId, dialog) ->
            dialog.closeFromFrontend()
            nativeEvent(sessionId, SoftwareKeyboardDialog.eventFrontendDestroyed, "", 0)
        }
    }

    fun open(sessionId : Long, buffer : ByteBuffer, initialText : String, inline : Boolean) : Boolean {
        diagnostic(sessionId, if (inline) "kotlin-open-inline" else "kotlin-open-normal", buffer.remaining())
        if (activity.isFinishing || activity.isDestroyed) {
            diagnostic(sessionId, "kotlin-open-rejected-activity", 0)
            return false
        }
        val config = try {
            buffer.order(ByteOrder.LITTLE_ENDIAN)
            ByteBufferSerializable.createFromByteBuffer(SoftwareKeyboardConfig::class, buffer) as SoftwareKeyboardConfig
        } catch (exception : Exception) {
            diagnostic(sessionId, "kotlin-open-rejected-config", 0)
            return false
        }

        activity.runOnUiThread {
            diagnostic(sessionId, "ui-open-entered", 0)
            if (activity.isFinishing || activity.isDestroyed) {
                diagnostic(sessionId, "ui-open-rejected-activity", 0)
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
            try {
                dialog.showNow(activity.supportFragmentManager, "software-keyboard-$sessionId")
                diagnostic(sessionId, "ui-showNow-complete", 0)
                nativeEvent(sessionId, SoftwareKeyboardDialog.eventFrontendOpened, "", 0)
            } catch (exception : IllegalStateException) {
                diagnostic(sessionId, "ui-open-rejected-fragment-state", 0)
                dialogs.remove(sessionId)
                dialog.closeFromFrontend()
                nativeEvent(sessionId, SoftwareKeyboardDialog.eventFrontendDestroyed, "", 0)
            }
        }
        return true
    }

    fun close(sessionId : Long) {
        activity.runOnUiThread {
            diagnostic(sessionId, "ui-close", if (dialogs.containsKey(sessionId)) 1 else 0)
            // Keep the session until the actual dialog/window reports dismissal.
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
        diagnostic(sessionId, "dialog-started", 0)
        val current = dialogs[sessionId]
        if (current !== dialog && dialogs.isNotEmpty()) {
            dialog.closeFromFrontend()
            return
        }
        dialogs[sessionId] = dialog
    }

    fun onDismissed(sessionId : Long, dialog : SoftwareKeyboardDialog) {
        diagnostic(sessionId, "dialog-dismissed", 0)
        if (dialogs[sessionId] === dialog) {
            dialogs.remove(sessionId)
        }
    }

    fun trace(sessionId : Long, dialog : SoftwareKeyboardDialog, stage : String, detail : Int) {
        if (dialogs[sessionId] === dialog)
            diagnostic(sessionId, stage, detail)
    }
}
