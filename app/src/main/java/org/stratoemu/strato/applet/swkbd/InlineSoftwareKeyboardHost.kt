/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.applet.swkbd

import android.content.Context
import android.graphics.Color
import android.os.Build
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.KeyEvent
import android.view.WindowInsets
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.FrameLayout
import androidx.core.widget.doOnTextChanged
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.stratoemu.strato.EmulationActivity

/**
 * Hosts Android's IME for inline SWKBD without drawing a second text-entry frontend.
 *
 * HOS inline software keyboards render their text through the game's indirect layer. Android still
 * needs a real, attached and focused editor to provide an InputConnection to the IME, so this keeps
 * a 1x1 transparent EditText in the activity content view instead of using a transparent dialog.
 */
class InlineSoftwareKeyboardHost(
    private val activity : EmulationActivity,
    private val sessionId : Long,
    initialConfig : SoftwareKeyboardConfig,
    initialText : String
) {
    private val textInput = EditText(activity)
    private val contentRoot = activity.findViewById<ViewGroup>(android.R.id.content)
    private val originalSoftInputMode = activity.window.attributes.softInputMode

    private var config = initialConfig
    private var multiline = false
    private var waitingForTextCheck = false
    private var suppressTextEvent = false
    private var visible = false
    private var closed = false

    init {
        applyConfig(initialConfig)

        // Inline SWKBD must use the activity's real window. A transparent Dialog can own focus
        // without becoming a reliable IME target on some Android devices, leaving the guest SWKBD
        // fully active while no keyboard is shown. Keep a real 1x1 editor attached directly to
        // android.R.id.content instead: it provides InputConnection without drawing a second UI.
        contentRoot.addView(textInput, FrameLayout.LayoutParams(1, 1, Gravity.TOP or Gravity.START))
        textInput.visibility = View.GONE

        // Keep the editor genuinely visible/focusable to Android while making its single pixel
        // impossible to notice over the game.
        textInput.setBackgroundColor(Color.TRANSPARENT)
        textInput.setTextColor(Color.TRANSPARENT)
        textInput.setHintTextColor(Color.TRANSPARENT)
        textInput.isCursorVisible = false
        textInput.setPadding(0, 0, 0, 0)
        textInput.gravity = Gravity.TOP or Gravity.START
        textInput.alpha = 1f

        suppressTextEvent = true
        textInput.setText(initialText)
        val initialCursor = if (config.initialCursorPos == InitialCursorPos.First) 0 else initialText.length
        textInput.setSelection(initialCursor.coerceIn(0, initialText.length))
        suppressTextEvent = false

        textInput.doOnTextChanged { value, _, _, _ ->
            if (!suppressTextEvent && !waitingForTextCheck)
                sendEvent(
                    SoftwareKeyboardDialog.eventTextChanged,
                    value?.toString().orEmpty(),
                    textInput.selectionStart.coerceAtLeast(0)
                )
        }

        textInput.setOnEditorActionListener { _, actionId, event ->
            val done = actionId == EditorInfo.IME_ACTION_DONE ||
                (!multiline && event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_UP)
            if (done)
                submit()
            done
        }
    }

    private fun applyConfig(newConfig : SoftwareKeyboardConfig) {
        config = newConfig
        multiline = config.inputFormMode == InputFormMode.MultiLine && config.isUseNewLine

        textInput.inputType = when {
            config.keyboardMode == KeyboardMode.Numeric && config.passwordMode == PasswordMode.Hide ->
                InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD
            config.keyboardMode == KeyboardMode.Numeric -> InputType.TYPE_CLASS_NUMBER
            config.passwordMode == PasswordMode.Hide ->
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            config.keyboardMode == KeyboardMode.ASCII ->
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            else -> InputType.TYPE_CLASS_TEXT
        }.let { type ->
            if (multiline)
                type or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            else
                type and InputType.TYPE_TEXT_FLAG_MULTI_LINE.inv()
        }
        textInput.isSingleLine = !multiline
        textInput.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or
            (if (multiline) EditorInfo.IME_ACTION_NONE else EditorInfo.IME_ACTION_DONE)
        textInput.filters = arrayOf(SoftwareKeyboardFilter(config))
    }

    fun reconfigure(newConfig : SoftwareKeyboardConfig) {
        if (!closed)
            applyConfig(newConfig)
    }

    fun show() {
        if (closed)
            return

        waitingForTextCheck = false
        textInput.isEnabled = true
        visible = true
        textInput.visibility = View.VISIBLE

        activity.window.apply {
            clearFlags(WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM)
            setSoftInputMode(
                WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE or
                    WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING
            )
        }

        showIme()
    }

    fun hide() {
        if (closed || !visible)
            return

        val inputMethod = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethod.hideSoftInputFromWindow(textInput.windowToken, 0)
        textInput.clearFocus()
        textInput.visibility = View.GONE
        visible = false
    }

    private fun showIme() {
        if (closed || !visible)
            return

        fun requestIme() {
            if (closed || !visible || !textInput.isAttachedToWindow)
                return

            textInput.isEnabled = true
            textInput.isFocusable = true
            textInput.isFocusableInTouchMode = true
            if (!textInput.hasFocus())
                textInput.requestFocus()
            if (!textInput.hasFocus() || !activity.window.decorView.hasWindowFocus())
                return

            val inputMethod = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            inputMethod.restartInput(textInput)
            inputMethod.showSoftInput(textInput, InputMethodManager.SHOW_IMPLICIT)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
                activity.window.insetsController?.show(WindowInsets.Type.ime())
        }

        // The editor is already attached to the activity window. Retry a few bounded times because
        // some IMEs attach one or two frames after focus changes.
        textInput.post {
            requestIme()
            textInput.postDelayed({ requestIme() }, 50)
            textInput.postDelayed({ requestIme() }, 150)
            textInput.postDelayed({ requestIme() }, 300)
        }
    }

    private fun submit() {
        if (waitingForTextCheck || closed || !config.isValid(textInput.text ?: ""))
            return

        waitingForTextCheck = true
        textInput.isEnabled = false
        sendEvent(
            SoftwareKeyboardDialog.eventSubmit,
            textInput.text?.toString().orEmpty(),
            textInput.selectionStart.coerceAtLeast(0)
        )
    }

    fun updateFromFrontend(text : String, cursor : Int) {
        if (closed)
            return
        suppressTextEvent = true
        textInput.setText(text)
        textInput.setSelection(cursor.coerceIn(0, text.length))
        suppressTextEvent = false
    }

    fun resumeEditing() {
        if (closed)
            return
        waitingForTextCheck = false
        textInput.isEnabled = true
        textInput.setSelection(textInput.text?.length ?: 0)
        showIme()
    }

    fun showTextCheck(result : Int, message : String) {
        if (closed)
            return

        val confirm = result == SoftwareKeyboardDialog.textCheckConfirm
        var resolved = false
        fun resolve(accepted : Boolean) {
            if (resolved || closed)
                return
            resolved = true
            sendEvent(
                if (accepted) SoftwareKeyboardDialog.eventTextCheckAccepted else SoftwareKeyboardDialog.eventTextCheckDismissed,
                textInput.text?.toString().orEmpty(),
                textInput.selectionStart.coerceAtLeast(0)
            )
        }

        MaterialAlertDialogBuilder(activity)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok) { _, _ -> resolve(confirm) }
            .apply {
                if (confirm)
                    setNegativeButton(android.R.string.cancel) { _, _ -> resolve(false) }
                setOnCancelListener { resolve(false) }
                setOnDismissListener { resolve(false) }
            }
            .show()
    }

    fun close() {
        if (closed)
            return

        val inputMethod = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethod.hideSoftInputFromWindow(textInput.windowToken, 0)
        textInput.clearFocus()
        textInput.visibility = View.GONE
        (textInput.parent as? ViewGroup)?.removeView(textInput)
        activity.window.setSoftInputMode(originalSoftInputMode)
        visible = false
        closed = true
    }

    private fun sendEvent(type : Int, text : String, cursor : Int) {
        activity.sendSoftwareKeyboardEvent(sessionId, type, text, cursor)
    }
}
