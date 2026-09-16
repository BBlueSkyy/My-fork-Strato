/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2026 Strato contributors
 */

package org.stratoemu.strato.applet.swkbd

import android.app.Activity
import android.content.Context
import android.os.Looper
import android.text.Editable
import android.text.InputType
import android.text.Selection
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import org.stratoemu.strato.utils.ByteBufferSerializable
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.FutureTask
import java.util.concurrent.LinkedBlockingQueue

/**
 * Invisible text-editor view used by inline SWKBD.
 *
 * Unlike the normal SoftwareKeyboardDialog, inline SWKBD is an Android IME frontend:
 * the view receives composing/committed text from the system keyboard and forwards
 * state changes to the HLE applet without blocking the emulation/UI thread.
 */
class InlineKeyboardInputView(context : Context) : View(context) {
    companion object {
        const val updateChanged = 0
        const val updateEnter = 1
        const val updateCancel = 2
        const val updateClosed = 3

        @JvmStatic
        fun show(activity : Activity, buffer : ByteBuffer, initialText : String) : InlineKeyboardInputView? {
            buffer.order(ByteOrder.LITTLE_ENDIAN)
            val config = ByteBufferSerializable.createFromByteBuffer(
                SoftwareKeyboardConfig::class,
                buffer
            ) as SoftwareKeyboardConfig

            val task = FutureTask<InlineKeyboardInputView?> {
                val view = InlineKeyboardInputView(activity)
                view.configure(config, initialText)

                val root = activity.findViewById<ViewGroup>(android.R.id.content) ?: return@FutureTask null
                root.addView(view, ViewGroup.LayoutParams(1, 1))
                view.activate()
                view
            }

            if (Looper.myLooper() == Looper.getMainLooper())
                task.run()
            else
                activity.runOnUiThread(task)

            return task.get()
        }

        @JvmStatic
        fun close(activity : Activity, view : InlineKeyboardInputView) {
            activity.runOnUiThread { view.deactivate() }
        }
    }

    private lateinit var config : SoftwareKeyboardConfig
    private val imeEditable : Editable = Editable.Factory.getInstance().newEditable("")
    private val updates = LinkedBlockingQueue<Array<Any?>>()

    @Volatile
    private var submitted = false

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    private fun configure(config : SoftwareKeyboardConfig, initialText : String) {
        this.config = config
        submitted = false
        updates.clear()
        imeEditable.replace(0, imeEditable.length, initialText)
        Selection.setSelection(imeEditable, imeEditable.length)
    }

    private fun activate() {
        visibility = VISIBLE
        requestFocus()
        post {
            val inputMethodManager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            inputMethodManager.restartInput(this)
            inputMethodManager.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
        }
    }

    private fun deactivate() {
        val inputMethodManager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.hideSoftInputFromWindow(windowToken, 0)
        clearFocus()
        cancelInlineWait()
        (parent as? ViewGroup)?.removeView(this)
    }

    override fun onCheckIsTextEditor() : Boolean = true

    override fun onCreateInputConnection(outAttrs : EditorInfo) : InputConnection {
        outAttrs.inputType = when (config.keyboardMode) {
            KeyboardMode.Numeric -> {
                if (config.passwordMode == PasswordMode.Hide)
                    InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD
                else
                    InputType.TYPE_CLASS_NUMBER
            }
            else -> {
                var type = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                if (config.passwordMode == PasswordMode.Hide)
                    type = type or InputType.TYPE_TEXT_VARIATION_PASSWORD
                if (config.isUseNewLine)
                    type = type or InputType.TYPE_TEXT_FLAG_MULTI_LINE
                type
            }
        }
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_ACTION_DONE
        outAttrs.initialSelStart = Selection.getSelectionStart(imeEditable).coerceAtLeast(0)
        outAttrs.initialSelEnd = Selection.getSelectionEnd(imeEditable).coerceAtLeast(0)

        return object : BaseInputConnection(this, true) {
            override fun getEditable() : Editable = imeEditable

            override fun commitText(text : CharSequence?, newCursorPosition : Int) : Boolean {
                val result = super.commitText(text, newCursorPosition)
                normalizeAndNotifyChanged()
                return result
            }

            override fun setComposingText(text : CharSequence?, newCursorPosition : Int) : Boolean {
                val result = super.setComposingText(text, newCursorPosition)
                normalizeAndNotifyChanged()
                return result
            }

            override fun deleteSurroundingText(beforeLength : Int, afterLength : Int) : Boolean {
                val result = super.deleteSurroundingText(beforeLength, afterLength)
                normalizeAndNotifyChanged()
                return result
            }

            override fun sendKeyEvent(event : KeyEvent) : Boolean {
                if (event.action != KeyEvent.ACTION_DOWN)
                    return true

                when (event.keyCode) {
                    KeyEvent.KEYCODE_ENTER,
                    KeyEvent.KEYCODE_BACK -> submitEnter()

                    KeyEvent.KEYCODE_DEL -> deleteBeforeCursor()
                    else -> {
                        val codepoint = event.unicodeChar
                        if (codepoint != 0)
                            insertAtCursor(String(Character.toChars(codepoint)))
                    }
                }
                return true
            }

            override fun performEditorAction(actionCode : Int) : Boolean {
                submitEnter()
                return true
            }
        }
    }

    override fun onKeyPreIme(keyCode : Int, event : KeyEvent) : Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_UP) {
            // Eden treats dismissal of the Android IME as completion of inline input.
            submitEnter()
            return true
        }
        return super.onKeyPreIme(keyCode, event)
    }

    private fun insertAtCursor(text : String) {
        val start = Selection.getSelectionStart(imeEditable).coerceAtLeast(0)
        val end = Selection.getSelectionEnd(imeEditable).coerceAtLeast(start)
        imeEditable.replace(start, end, text)
        Selection.setSelection(imeEditable, (start + text.length).coerceAtMost(imeEditable.length))
        normalizeAndNotifyChanged()
    }

    private fun deleteBeforeCursor() {
        val start = Selection.getSelectionStart(imeEditable).coerceAtLeast(0)
        val end = Selection.getSelectionEnd(imeEditable).coerceAtLeast(start)
        when {
            end > start -> imeEditable.delete(start, end)
            start > 0 -> imeEditable.delete(start - 1, start)
            else -> return
        }
        Selection.setSelection(imeEditable, start.coerceAtMost(imeEditable.length))
        normalizeAndNotifyChanged()
    }

    private fun normalizeAndNotifyChanged() {
        val maxLength = config.textMaxLength.toInt()
        if (maxLength > 0 && imeEditable.length > maxLength)
            imeEditable.delete(maxLength, imeEditable.length)

        val cursor = Selection.getSelectionEnd(imeEditable).let {
            if (it < 0) imeEditable.length else it.coerceIn(0, imeEditable.length)
        }
        updates.offer(arrayOf(updateChanged, imeEditable.toString(), cursor))
    }

    private fun submitEnter() {
        if (submitted)
            return
        submitted = true
        val cursor = Selection.getSelectionEnd(imeEditable).let {
            if (it < 0) imeEditable.length else it.coerceIn(0, imeEditable.length)
        }
        updates.offer(arrayOf(updateEnter, imeEditable.toString(), cursor))
    }

    fun waitForInlineUpdate() : Array<Any?> = updates.take()

    fun cancelInlineWait() {
        updates.offer(arrayOf(updateClosed, "", 0))
    }
}
