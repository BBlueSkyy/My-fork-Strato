/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2026 Strato contributors
 */

package org.stratoemu.strato.applet.swkbd

import android.app.Activity
import android.content.Context
import android.text.Editable
import android.text.InputType
import android.text.Selection
import android.util.AttributeSet
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import org.stratoemu.strato.R
import org.stratoemu.strato.utils.ByteBufferSerializable
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.LinkedBlockingQueue

/**
 * Permanently attached text-editor View used by inline SWKBD.
 *
 * Eden opens Android's IME on an already-attached emulation overlay. Doing the same here avoids
 * relying on a transient 1x1 View being attached/focused at exactly the right point in the applet
 * lifecycle.
 */
class InlineKeyboardInputView @JvmOverloads constructor(
    context : Context,
    attrs : AttributeSet? = null
) : View(context, attrs) {
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

            val view = activity.findViewById<InlineKeyboardInputView>(R.id.inline_keyboard_input_view)
                ?: return null

            view.prepareSession(config, initialText)
            activity.runOnUiThread { view.activate() }
            return view
        }

        @JvmStatic
        fun close(activity : Activity, session : InlineKeyboardInputView) {
            session.closeSession(activity)
        }
    }

    private val updates = LinkedBlockingQueue<Array<Any?>>()
    private var activeConfig : SoftwareKeyboardConfig? = null
    private var imeEditable : Editable = Editable.Factory.getInstance().newEditable("")

    @Volatile
    private var closed = true

    @Volatile
    private var submitted = false

    private fun prepareSession(config : SoftwareKeyboardConfig, initialText : String) {
        updates.clear()
        activeConfig = config
        imeEditable = Editable.Factory.getInstance().newEditable(initialText)
        Selection.setSelection(imeEditable, imeEditable.length)
        submitted = false
        closed = false
    }

    private fun activate() {
        if (closed)
            return

        visibility = VISIBLE
        isFocusable = true
        isFocusableInTouchMode = true

        post {
            if (closed || !isAttachedToWindow)
                return@post

            requestFocus()
            val inputMethodManager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            inputMethodManager.restartInput(this)
            inputMethodManager.showSoftInput(this, InputMethodManager.SHOW_FORCED)
        }
    }

    private fun closeSession(activity : Activity) {
        if (closed)
            return

        closed = true
        updates.offer(arrayOf(updateClosed, "", 0))
        activity.runOnUiThread { deactivate() }
    }

    private fun deactivate() {
        val inputMethodManager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.hideSoftInputFromWindow(windowToken, 0)
        inputMethodManager.restartInput(this)
        clearFocus()
        visibility = GONE
        activeConfig = null
    }

    private fun postChanged() {
        if (!closed && !submitted)
            updates.offer(arrayOf(updateChanged, imeEditable.toString(), currentCursor()))
    }

    private fun postEnter() {
        if (closed || submitted)
            return

        submitted = true
        updates.offer(arrayOf(updateEnter, imeEditable.toString(), currentCursor()))
    }

    fun waitForInlineUpdate() : Array<Any?> = updates.take()

    fun cancelInlineWait() {
        updates.offer(arrayOf(updateClosed, "", 0))
    }

    override fun onCheckIsTextEditor() : Boolean = !closed && activeConfig != null

    override fun onCreateInputConnection(outAttrs : EditorInfo) : InputConnection? {
        val config = activeConfig ?: return null

        outAttrs.inputType = when (config.keyboardMode) {
            KeyboardMode.Numeric -> {
                if (config.passwordMode == PasswordMode.Hide)
                    InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD
                else
                    InputType.TYPE_CLASS_NUMBER
            }
            else -> {
                var type = InputType.TYPE_CLASS_TEXT or
                    InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
                    InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                if (config.passwordMode == PasswordMode.Hide)
                    type = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
                if (config.isUseNewLine)
                    type = type or InputType.TYPE_TEXT_FLAG_MULTI_LINE
                type
            }
        }

        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_ACTION_DONE
        outAttrs.initialSelStart = currentCursor()
        outAttrs.initialSelEnd = currentCursor()

        return object : BaseInputConnection(this, true) {
            override fun getEditable() : Editable = imeEditable

            override fun commitText(text : CharSequence?, newCursorPosition : Int) : Boolean {
                val result = super.commitText(text, newCursorPosition)
                normalizeAndNotifyChanged(config)
                return result
            }

            override fun setComposingText(text : CharSequence?, newCursorPosition : Int) : Boolean {
                val result = super.setComposingText(text, newCursorPosition)
                normalizeAndNotifyChanged(config)
                return result
            }

            override fun deleteSurroundingText(beforeLength : Int, afterLength : Int) : Boolean {
                val result = super.deleteSurroundingText(beforeLength, afterLength)
                normalizeAndNotifyChanged(config)
                return result
            }

            override fun sendKeyEvent(event : KeyEvent) : Boolean {
                if (event.action != KeyEvent.ACTION_DOWN)
                    return true

                when (event.keyCode) {
                    KeyEvent.KEYCODE_ENTER,
                    KeyEvent.KEYCODE_BACK -> postEnter()

                    KeyEvent.KEYCODE_DEL -> deleteBeforeCursor(config)
                    else -> {
                        val codepoint = event.unicodeChar
                        if (codepoint != 0)
                            insertAtCursor(String(Character.toChars(codepoint)), config)
                    }
                }
                return true
            }

            override fun performEditorAction(actionCode : Int) : Boolean {
                postEnter()
                return true
            }
        }
    }

    override fun onKeyPreIme(keyCode : Int, event : KeyEvent) : Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_UP) {
            postEnter()
            return true
        }
        return super.onKeyPreIme(keyCode, event)
    }

    private fun insertAtCursor(text : String, config : SoftwareKeyboardConfig) {
        val start = Selection.getSelectionStart(imeEditable).coerceAtLeast(0)
        val end = Selection.getSelectionEnd(imeEditable).coerceAtLeast(start)
        imeEditable.replace(start, end, text)
        Selection.setSelection(imeEditable, (start + text.length).coerceAtMost(imeEditable.length))
        normalizeAndNotifyChanged(config)
    }

    private fun deleteBeforeCursor(config : SoftwareKeyboardConfig) {
        val start = Selection.getSelectionStart(imeEditable).coerceAtLeast(0)
        val end = Selection.getSelectionEnd(imeEditable).coerceAtLeast(start)
        when {
            end > start -> imeEditable.delete(start, end)
            start > 0 -> imeEditable.delete(start - 1, start)
            else -> return
        }
        Selection.setSelection(imeEditable, start.coerceAtMost(imeEditable.length))
        normalizeAndNotifyChanged(config)
    }

    private fun normalizeAndNotifyChanged(config : SoftwareKeyboardConfig) {
        val maxLength = config.textMaxLength.toInt()
        if (maxLength > 0 && imeEditable.length > maxLength)
            imeEditable.delete(maxLength, imeEditable.length)

        val cursor = currentCursor()
        Selection.setSelection(imeEditable, cursor)
        postChanged()
    }

    private fun currentCursor() : Int {
        return Selection.getSelectionEnd(imeEditable).let {
            if (it < 0) imeEditable.length else it.coerceIn(0, imeEditable.length)
        }
    }
}
