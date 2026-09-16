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
import java.util.concurrent.LinkedBlockingQueue

/**
 * Asynchronous Android IME session used by inline SWKBD.
 *
 * The session object itself is safe to create from the emulation/JNI thread. The actual
 * text-editor View is attached on the Android UI thread, matching Eden's inline frontend
 * model and avoiding any synchronous wait on the UI thread from HLE IPC processing.
 */
class InlineKeyboardInputView private constructor(
    private val activity : Activity,
    private val config : SoftwareKeyboardConfig,
    initialText : String
) {
    companion object {
        const val updateChanged = 0
        const val updateEnter = 1
        const val updateCancel = 2
        const val updateClosed = 3

        @JvmStatic
        fun show(activity : Activity, buffer : ByteBuffer, initialText : String) : InlineKeyboardInputView {
            buffer.order(ByteOrder.LITTLE_ENDIAN)
            val config = ByteBufferSerializable.createFromByteBuffer(
                SoftwareKeyboardConfig::class,
                buffer
            ) as SoftwareKeyboardConfig

            return InlineKeyboardInputView(activity, config, initialText).also { session ->
                activity.runOnUiThread { session.attachAndShow() }
            }
        }

        @JvmStatic
        fun close(activity : Activity, session : InlineKeyboardInputView) {
            session.close()
        }
    }

    private val updates = LinkedBlockingQueue<Array<Any?>>()

    @Volatile
    private var closed = false

    @Volatile
    private var submitted = false

    private var editorView : ImeEditorView? = null
    private val initialText = initialText

    private fun attachAndShow() {
        if (closed)
            return

        val root = activity.findViewById<ViewGroup>(android.R.id.content) ?: run {
            cancelInlineWait()
            return
        }

        val view = ImeEditorView(activity, this, config, initialText)
        editorView = view
        root.addView(view, ViewGroup.LayoutParams(1, 1))
        view.activate()
    }

    private fun close() {
        if (closed)
            return
        closed = true

        // Unblock the native waiter immediately; Android view teardown remains UI-thread-only.
        cancelInlineWait()
        activity.runOnUiThread {
            editorView?.deactivate()
            editorView = null
        }
    }

    private fun postChanged(text : String, cursor : Int) {
        if (!closed && !submitted)
            updates.offer(arrayOf(updateChanged, text, cursor))
    }

    private fun postEnter(text : String, cursor : Int) {
        if (closed || submitted)
            return
        submitted = true
        updates.offer(arrayOf(updateEnter, text, cursor))
    }

    fun waitForInlineUpdate() : Array<Any?> = updates.take()

    fun cancelInlineWait() {
        updates.offer(arrayOf(updateClosed, "", 0))
    }

    private class ImeEditorView(
        context : Context,
        private val session : InlineKeyboardInputView,
        private val config : SoftwareKeyboardConfig,
        initialText : String
    ) : View(context) {
        private val imeEditable : Editable = Editable.Factory.getInstance().newEditable(initialText)

        init {
            isFocusable = true
            isFocusableInTouchMode = true
            Selection.setSelection(imeEditable, imeEditable.length)
        }

        fun activate() {
            visibility = VISIBLE
            requestFocus()
            post {
                val inputMethodManager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
                inputMethodManager.restartInput(this)
                inputMethodManager.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
            }
        }

        fun deactivate() {
            val inputMethodManager = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            inputMethodManager.hideSoftInputFromWindow(windowToken, 0)
            clearFocus()
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
                // Eden treats Android IME dismissal as completion for inline input.
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

            val cursor = currentCursor()
            session.postChanged(imeEditable.toString(), cursor)
        }

        private fun submitEnter() {
            session.postEnter(imeEditable.toString(), currentCursor())
        }

        private fun currentCursor() : Int {
            return Selection.getSelectionEnd(imeEditable).let {
                if (it < 0) imeEditable.length else it.coerceIn(0, imeEditable.length)
            }
        }
    }
}
