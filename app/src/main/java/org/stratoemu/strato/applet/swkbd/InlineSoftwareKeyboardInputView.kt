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
 * Full-screen, transparent InputConnection host for inline SWKBD.
 *
 * Eden attaches the Android IME to its emulation input overlay rather than creating
 * an EditText/dialog for inline mode. Keep this view permanently attached to the
 * emulation overlay for the same reason: it is only an IME/InputConnection host;
 * the game renders the visible text itself.
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

    // Eden keeps its emulation input overlay as a text editor all the time.
    override fun onCheckIsTextEditor() : Boolean = true

    override fun onCreateInputConnection(outAttrs : EditorInfo) : InputConnection {
        // Match Eden's inline Android IME contract: plain text, no suggestions,
        // visible-password variation and a Done action. Inline games render the
        // actual text themselves.
        outAttrs.inputType =
            InputType.TYPE_CLASS_TEXT or
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
                InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        outAttrs.imeOptions =
            EditorInfo.IME_FLAG_NO_EXTRACT_UI or
                EditorInfo.IME_FLAG_NO_FULLSCREEN or
                EditorInfo.IME_ACTION_DONE

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
                    KeyEvent.KEYCODE_BACK,
                    KeyEvent.KEYCODE_ENTER -> {
                        // Eden treats dismiss/Back and Enter as DecidedEnter for inline SWKBD.
                        sendSubmit()
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
        eventSink = sink

        val initialCursor =
            if (keyboardConfig.initialCursorPos == InitialCursorPos.First) 0 else initialText.length
        replaceText(initialText, initialCursor)

        imeWasVisible = false
        submitOnDismiss = true
        handler.removeCallbacks(imeVisibilityPoll)

        post {
            if (activeSessionId != sessionId || !isAttachedToWindow)
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

        post {
            if (activeSessionId != sessionId || !isAttachedToWindow)
                return@post

            requestFocus()
            inputMethodManager.restartInput(this)
            inputMethodManager.showSoftInput(this, InputMethodManager.SHOW_FORCED)
            handler.postDelayed(imeVisibilityPoll, 500)
        }
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
        eventSink = null
        editable.clear()
    }

    fun updateSession(sessionId : Long, text : String, cursor : Int) {
        if (activeSessionId != sessionId)
            return

        replaceText(text, cursor)

        // Do NOT restart the InputConnection here. Eden keeps the same IME session
        // alive while the guest changes inline text/cursor state.
        if (hasFocus) {
            val position = currentCursor()
            inputMethodManager.updateSelection(this, position, position, -1, -1)
        }
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
        if (activeSessionId == null)
            return
        eventSink?.invoke(
            SoftwareKeyboardDialog.eventTextChanged,
            editable.toString(),
            currentCursor()
        )
    }

    private fun sendSubmit() {
        if (activeSessionId == null)
            return
        eventSink?.invoke(
            SoftwareKeyboardDialog.eventSubmit,
            editable.toString(),
            currentCursor()
        )
    }

    override fun onDetachedFromWindow() {
        handler.removeCallbacks(imeVisibilityPoll)
        activeSessionId = null
        eventSink = null
        super.onDetachedFromWindow()
    }
}
