/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2026 Strato Team and Contributors
 */

package org.stratoemu.strato.applet.swkbd

import android.content.Context
import android.os.Handler
import android.os.Looper
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
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat

/**
 * Invisible InputConnection host for inline SWKBD.
 *
 * This follows Eden's Android design: inline SWKBD does not create a dialog or EditText.
 * A normal View becomes the active text editor and the Android IME is shown directly over
 * the emulation surface. The game renders the actual text through its indirect layer.
 */
class InlineSoftwareKeyboardInputView @JvmOverloads constructor(
    context : Context,
    attrs : AttributeSet? = null,
    defStyleAttr : Int = 0
) : View(context, attrs, defStyleAttr) {
    private val editable : Editable = Editable.Factory.getInstance().newEditable("")
    private val handler = Handler(Looper.getMainLooper())
    private val inputMethodManager =
        context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager

    private var config : SoftwareKeyboardConfig? = null
    private var eventSink : ((type : Int, text : String, cursor : Int) -> Unit)? = null
    private var imeWasVisible = false
    private var submitOnDismiss = false

    var activeSessionId : Long? = null
        private set

    private val imeVisibilityPoll = object : Runnable {
        override fun run() {
            if (activeSessionId == null || !isAttachedToWindow)
                return

            val visible = ViewCompat.getRootWindowInsets(this@InlineSoftwareKeyboardInputView)
                ?.isVisible(WindowInsetsCompat.Type.ime()) == true
            if (visible) {
                imeWasVisible = true
                handler.postDelayed(this, 500)
                return
            }

            // Same lifecycle rule used by Eden: dismissing the Android IME means Enter.
            if (imeWasVisible && submitOnDismiss) {
                submitOnDismiss = false
                sendSubmit()
                return
            }

            handler.postDelayed(this, 500)
        }
    }

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onCheckIsTextEditor() : Boolean = activeSessionId != null

    override fun onCreateInputConnection(outAttrs : EditorInfo) : InputConnection {
        val currentConfig = config
        val multiline = currentConfig?.let {
            it.inputFormMode == InputFormMode.MultiLine && it.isUseNewLine
        } == true

        outAttrs.inputType = when {
            currentConfig?.keyboardMode == KeyboardMode.Numeric &&
                currentConfig.passwordMode == PasswordMode.Hide ->
                InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD
            currentConfig?.keyboardMode == KeyboardMode.Numeric ->
                InputType.TYPE_CLASS_NUMBER
            currentConfig?.passwordMode == PasswordMode.Hide ->
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            currentConfig?.keyboardMode == KeyboardMode.ASCII ->
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
                    InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            else ->
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        }.let { type ->
            if (multiline)
                type or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            else
                type and InputType.TYPE_TEXT_FLAG_MULTI_LINE.inv()
        }
        outAttrs.imeOptions =
            EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or
                (if (multiline) EditorInfo.IME_ACTION_NONE else EditorInfo.IME_ACTION_DONE)

        val cursor = currentCursor()
        outAttrs.initialSelStart = cursor
        outAttrs.initialSelEnd = cursor

        return object : BaseInputConnection(this, true) {
            override fun getEditable() : Editable = editable

            override fun commitText(text : CharSequence?, newCursorPosition : Int) : Boolean {
                val result = super.commitText(text, newCursorPosition)
                sendChanged()
                return result
            }

            override fun deleteSurroundingText(beforeLength : Int, afterLength : Int) : Boolean {
                val result = super.deleteSurroundingText(beforeLength, afterLength)
                sendChanged()
                return result
            }

            override fun sendKeyEvent(event : KeyEvent) : Boolean {
                if (event.action != KeyEvent.ACTION_DOWN)
                    return true

                return when (event.keyCode) {
                    KeyEvent.KEYCODE_ENTER -> {
                        sendSubmit()
                        true
                    }
                    KeyEvent.KEYCODE_BACK -> {
                        if (config?.isCancelButtonDisabled != true)
                            sendCancel()
                        true
                    }
                    KeyEvent.KEYCODE_DEL -> {
                        deleteSurroundingText(1, 0)
                        true
                    }
                    else -> {
                        val codePoint = event.unicodeChar
                        if (codePoint != 0) {
                            commitText(String(Character.toChars(codePoint)), 1)
                            true
                        } else {
                            super.sendKeyEvent(event)
                        }
                    }
                }
            }

            override fun performEditorAction(actionCode : Int) : Boolean {
                sendSubmit()
                return true
            }
        }
    }

    fun openSession(
        sessionId : Long,
        keyboardConfig : SoftwareKeyboardConfig,
        initialText : String,
        sink : (type : Int, text : String, cursor : Int) -> Unit
    ) {
        activeSessionId = sessionId
        config = keyboardConfig
        eventSink = sink
        replaceText(initialText, if (keyboardConfig.initialCursorPos == InitialCursorPos.First) 0 else initialText.length)

        imeWasVisible = false
        submitOnDismiss = true
        handler.removeCallbacks(imeVisibilityPoll)

        post {
            if (activeSessionId != sessionId)
                return@post
            requestFocus()
            inputMethodManager.restartInput(this)
            inputMethodManager.showSoftInput(this, InputMethodManager.SHOW_FORCED)
            handler.postDelayed(imeVisibilityPoll, 500)
        }
    }

    fun showSession(sessionId : Long) {
        if (activeSessionId != sessionId)
            return
        imeWasVisible = false
        submitOnDismiss = true
        handler.removeCallbacks(imeVisibilityPoll)
        requestFocus()
        inputMethodManager.restartInput(this)
        inputMethodManager.showSoftInput(this, InputMethodManager.SHOW_FORCED)
        handler.postDelayed(imeVisibilityPoll, 500)
    }

    fun hideSession(sessionId : Long) {
        if (activeSessionId != sessionId)
            return
        submitOnDismiss = false
        handler.removeCallbacks(imeVisibilityPoll)
        inputMethodManager.hideSoftInputFromWindow(windowToken, 0)
        clearFocus()
    }

    fun closeSession(sessionId : Long) {
        if (activeSessionId != sessionId)
            return
        hideSession(sessionId)
        activeSessionId = null
        config = null
        eventSink = null
        editable.clear()
    }

    fun updateSession(sessionId : Long, text : String, cursor : Int) {
        if (activeSessionId != sessionId)
            return
        replaceText(text, cursor)
        inputMethodManager.restartInput(this)
    }

    private fun replaceText(text : String, cursor : Int) {
        editable.replace(0, editable.length, text)
        Selection.setSelection(editable, cursor.coerceIn(0, editable.length))
    }

    private fun currentCursor() : Int {
        val cursor = Selection.getSelectionEnd(editable)
        return if (cursor >= 0) cursor else editable.length
    }

    private fun sendChanged() {
        eventSink?.invoke(
            SoftwareKeyboardDialog.eventTextChanged,
            editable.toString(),
            currentCursor()
        )
    }

    private fun sendSubmit() {
        eventSink?.invoke(
            SoftwareKeyboardDialog.eventSubmit,
            editable.toString(),
            currentCursor()
        )
    }

    private fun sendCancel() {
        eventSink?.invoke(
            SoftwareKeyboardDialog.eventCancel,
            editable.toString(),
            currentCursor()
        )
    }

    override fun onDetachedFromWindow() {
        handler.removeCallbacks(imeVisibilityPoll)
        activeSessionId = null
        config = null
        eventSink = null
        super.onDetachedFromWindow()
    }
}
