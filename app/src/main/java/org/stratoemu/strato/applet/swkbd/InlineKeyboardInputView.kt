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

class InlineKeyboardInputView(context : Context) : View(context) {
    companion object {
        const val EVENT_CHANGED = 0
        const val EVENT_MOVED = 1
        const val EVENT_ENTER = 2
        const val EVENT_CANCEL = 3

        private var current : InlineKeyboardInputView? = null

        @JvmStatic
        external fun submitInlineKeyboardEvent(kind : Int, text : String, cursor : Int)

        @JvmStatic
        fun show(activity : Activity, buffer : ByteBuffer, initialText : String, cursor : Int, enableBackspace : Boolean) {
            buffer.order(ByteOrder.LITTLE_ENDIAN)
            val config = ByteBufferSerializable.createFromByteBuffer(
                SoftwareKeyboardConfig::class,
                buffer
            ) as SoftwareKeyboardConfig

            activity.runOnUiThread {
                val root = activity.findViewById<ViewGroup>(android.R.id.content)
                val view = current ?: InlineKeyboardInputView(activity).also {
                    root.addView(it, ViewGroup.LayoutParams(1, 1))
                    current = it
                }
                view.open(config, initialText, cursor, enableBackspace)
            }
        }

        @JvmStatic
        fun update(activity : Activity, text : String, cursor : Int) {
            activity.runOnUiThread { current?.setGuestText(text, cursor) }
        }

        @JvmStatic
        fun hide(activity : Activity) {
            activity.runOnUiThread { current?.hideIme() }
        }

        @JvmStatic
        fun close(activity : Activity) {
            activity.runOnUiThread {
                current?.let { view ->
                    view.closeSession()
                    (view.parent as? ViewGroup)?.removeView(view)
                }
                current = null
            }
        }
    }

    private var config : SoftwareKeyboardConfig? = null
    private var editable : Editable = Editable.Factory.getInstance().newEditable("")
    private var enableBackspace = true
    private var suppressCallbacks = false
    private var submitted = false

    private fun open(config : SoftwareKeyboardConfig, text : String, cursor : Int, enableBackspace : Boolean) {
        this.config = config
        this.enableBackspace = enableBackspace
        submitted = false
        setGuestText(text, cursor)

        visibility = VISIBLE
        alpha = 0f
        isFocusable = true
        isFocusableInTouchMode = true
        requestFocus()

        val inputMethodManager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.restartInput(this)
        inputMethodManager.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
    }

    private fun setGuestText(text : String, cursor : Int) {
        suppressCallbacks = true
        val maxLength = config?.textMaxLength?.toInt()?.takeIf { it > 0 } ?: 500
        val clampedText = text.take(maxLength)
        editable = Editable.Factory.getInstance().newEditable(clampedText)
        Selection.setSelection(editable, cursor.coerceIn(0, editable.length))
        suppressCallbacks = false

        if (isFocused) {
            val inputMethodManager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            inputMethodManager.restartInput(this)
        }
    }

    private fun hideIme() {
        val inputMethodManager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.hideSoftInputFromWindow(windowToken, 0)
        clearFocus()
        visibility = INVISIBLE
    }

    private fun closeSession() {
        hideIme()
        config = null
        submitted = true
    }

    private fun cursor() : Int {
        val selection = Selection.getSelectionEnd(editable)
        return if (selection < 0) editable.length else selection.coerceIn(0, editable.length)
    }

    private fun normalize() {
        val maxLength = config?.textMaxLength?.toInt()?.takeIf { it > 0 } ?: 500
        if (editable.length > maxLength)
            editable.delete(maxLength, editable.length)
        Selection.setSelection(editable, cursor().coerceAtMost(editable.length))
    }

    private fun changed() {
        if (suppressCallbacks || submitted)
            return
        normalize()
        submitInlineKeyboardEvent(EVENT_CHANGED, editable.toString(), cursor())
    }

    private fun moved() {
        if (!suppressCallbacks && !submitted)
            submitInlineKeyboardEvent(EVENT_MOVED, editable.toString(), cursor())
    }

    private fun finish(kind : Int) {
        if (submitted)
            return
        submitted = true
        submitInlineKeyboardEvent(kind, editable.toString(), cursor())
    }

    override fun onCheckIsTextEditor() : Boolean = config != null && visibility == VISIBLE

    override fun onCreateInputConnection(outAttrs : EditorInfo) : InputConnection? {
        val activeConfig = config ?: return null

        outAttrs.inputType = when (activeConfig.keyboardMode) {
            KeyboardMode.Numeric -> {
                if (activeConfig.passwordMode == PasswordMode.Hide)
                    InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD
                else
                    InputType.TYPE_CLASS_NUMBER
            }
            else -> {
                if (activeConfig.passwordMode == PasswordMode.Hide)
                    InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
                else
                    InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
            }
        }
        if (activeConfig.isUseNewLine)
            outAttrs.inputType = outAttrs.inputType or InputType.TYPE_TEXT_FLAG_MULTI_LINE

        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_ACTION_DONE
        outAttrs.initialSelStart = cursor()
        outAttrs.initialSelEnd = cursor()

        return object : BaseInputConnection(this@InlineKeyboardInputView, true) {
            override fun getEditable() : Editable = this@InlineKeyboardInputView.editable

            override fun commitText(text : CharSequence?, newCursorPosition : Int) : Boolean {
                val result = super.commitText(text, newCursorPosition)
                changed()
                return result
            }

            override fun setComposingText(text : CharSequence?, newCursorPosition : Int) : Boolean {
                val result = super.setComposingText(text, newCursorPosition)
                changed()
                return result
            }

            override fun deleteSurroundingText(beforeLength : Int, afterLength : Int) : Boolean {
                if (!enableBackspace)
                    return true
                val result = super.deleteSurroundingText(beforeLength, afterLength)
                changed()
                return result
            }

            override fun setSelection(start : Int, end : Int) : Boolean {
                val result = super.setSelection(start, end)
                moved()
                return result
            }

            override fun sendKeyEvent(event : KeyEvent) : Boolean {
                if (event.action != KeyEvent.ACTION_DOWN)
                    return true

                return when (event.keyCode) {
                    KeyEvent.KEYCODE_ENTER -> {
                        finish(EVENT_ENTER)
                        true
                    }
                    KeyEvent.KEYCODE_BACK -> {
                        if (!activeConfig.isCancelButtonDisabled)
                            finish(EVENT_CANCEL)
                        true
                    }
                    KeyEvent.KEYCODE_DEL -> {
                        if (enableBackspace)
                            deleteSurroundingText(1, 0)
                        true
                    }
                    else -> {
                        val result = super.sendKeyEvent(event)
                        if (result)
                            changed()
                        result
                    }
                }
            }

            override fun performEditorAction(actionCode : Int) : Boolean {
                finish(EVENT_ENTER)
                return true
            }
        }
    }

    override fun onKeyPreIme(keyCode : Int, event : KeyEvent) : Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_UP) {
            if (config?.isCancelButtonDisabled != true)
                finish(EVENT_CANCEL)
            return true
        }
        return super.onKeyPreIme(keyCode, event)
    }
}
