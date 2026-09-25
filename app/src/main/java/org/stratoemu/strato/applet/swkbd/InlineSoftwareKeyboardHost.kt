/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.applet.swkbd

import android.app.Dialog
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.os.Build
import android.text.InputType
import android.view.Gravity
import android.view.KeyEvent
import android.view.Window
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
    private val container = FrameLayout(activity)
    private val dialog = Dialog(activity)

    private var config = initialConfig
    private var multiline = false
    private var waitingForTextCheck = false
    private var suppressTextEvent = false
    private var closed = false

    init {
        applyConfig(initialConfig)

        // Give the IME its own focused Android window without drawing an emulator text-entry UI.
        // A plain transparent Dialog is intentionally used instead of a Material dialog: the only
        // child is the 1x1 editor that provides InputConnection to the Android IME.
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE)
        container.setBackgroundColor(Color.TRANSPARENT)
        container.addView(textInput, FrameLayout.LayoutParams(1, 1, Gravity.TOP or Gravity.START))
        dialog.setContentView(container)
        dialog.setCancelable(false)
        dialog.setCanceledOnTouchOutside(false)
        dialog.window?.apply {
            clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            setDimAmount(0f)
            setGravity(Gravity.TOP or Gravity.START)
            setSoftInputMode(
                WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE or
                    WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING
            )
        }

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

        // Dialog.hide() intentionally keeps the Dialog instance alive and isShowing remains
        // true. Calling show() again is what makes its decor visible after a previous hide(), so
        // this must not be gated on isShowing or inline SWKBD only works the first time.
        dialog.show()
        dialog.window?.apply {
            clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            setDimAmount(0f)
            setGravity(Gravity.TOP or Gravity.START)
            setLayout(1, 1)
            setSoftInputMode(
                WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE or
                    WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING
            )
        }

        showIme()
    }

    fun hide() {
        if (closed || !dialog.isShowing)
            return

        val inputMethod = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethod.hideSoftInputFromWindow(textInput.windowToken, 0)
        textInput.clearFocus()
        dialog.hide()
    }

    private fun showIme() {
        if (closed || !dialog.isShowing)
            return

        val window = dialog.window ?: return
        val decorView = window.decorView

        fun requestIme() {
            if (closed || !dialog.isShowing)
                return

            textInput.isEnabled = true
            textInput.isFocusableInTouchMode = true
            if (!textInput.hasFocus())
                textInput.requestFocus()
            if (!textInput.hasFocus() || !decorView.hasWindowFocus())
                return

            val inputMethod = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            inputMethod.restartInput(textInput)
            inputMethod.showSoftInput(textInput, InputMethodManager.SHOW_IMPLICIT)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
                window.insetsController?.show(WindowInsets.Type.ime())
        }

        // The dialog supplies a real focused window to the IME. Request once when that window has
        // focus and keep only two bounded retries for devices where IMM attachment lags a frame.
        decorView.post {
            requestIme()
            decorView.postDelayed({ requestIme() }, 100)
            decorView.postDelayed({ requestIme() }, 250)
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

        if (dialog.isShowing) {
            val inputMethod = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            inputMethod.hideSoftInputFromWindow(textInput.windowToken, 0)
            textInput.clearFocus()
            dialog.dismiss()
        }
        closed = true
    }

    private fun sendEvent(type : Int, text : String, cursor : Int) {
        activity.sendSoftwareKeyboardEvent(sessionId, type, text, cursor)
    }
}
