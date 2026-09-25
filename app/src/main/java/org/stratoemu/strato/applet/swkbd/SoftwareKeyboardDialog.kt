/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.applet.swkbd

import android.annotation.SuppressLint
import android.content.Context
import android.content.DialogInterface
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.os.Bundle
import android.text.InputType
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.ViewTreeObserver
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import androidx.core.widget.doOnTextChanged
import androidx.fragment.app.DialogFragment
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.TextInputLayout
import org.stratoemu.strato.EmulationActivity
import org.stratoemu.strato.databinding.KeyboardDialogBinding
import org.stratoemu.strato.utils.parcelable
import org.stratoemu.strato.utils.stringFromChars

class SoftwareKeyboardDialog : DialogFragment() {
    private val sessionId by lazy { requireArguments().getLong(argumentSessionId) }
    private val config by lazy { requireArguments().parcelable<SoftwareKeyboardConfig>(argumentConfig)!! }
    private val initialText by lazy { requireArguments().getString(argumentInitialText)!! }
    private val inline by lazy { requireArguments().getBoolean(argumentInline) }

    private lateinit var binding : KeyboardDialogBinding
    private var pendingText : String? = null
    private var closedByFrontend = false
    private var terminalEventSent = false
    private var waitingForTextCheck = false
    private var suppressTextEvent = false
    private var imeWindowFocusListener : ViewTreeObserver.OnWindowFocusChangeListener? = null

    companion object {
        private const val argumentSessionId = "sessionId"
        private const val argumentConfig = "config"
        private const val argumentInitialText = "initialText"
        private const val argumentInline = "inline"

        const val textCheckFailure = 1
        const val textCheckConfirm = 2

        const val eventTextChanged = 0
        const val eventSubmit = 1
        const val eventCancel = 2
        const val eventTextCheckAccepted = 3
        const val eventTextCheckDismissed = 4
        const val eventFrontendDestroyed = 5

        fun newInstance(sessionId : Long, config : SoftwareKeyboardConfig, initialText : String,
                        inline : Boolean) = SoftwareKeyboardDialog().apply {
            arguments = Bundle().apply {
                putLong(argumentSessionId, sessionId)
                putParcelable(argumentConfig, config)
                putString(argumentInitialText, initialText)
                putBoolean(argumentInline, inline)
            }
        }
    }

    override fun onCreateView(inflater : LayoutInflater, container : ViewGroup?, savedInstanceState : Bundle?) =
        KeyboardDialogBinding.inflate(inflater, container, false).also { binding = it }.root

    @SuppressLint("SetTextI18n")
    override fun onViewCreated(view : View, savedInstanceState : Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        isCancelable = !config.isCancelButtonDisabled
        dialog?.setCanceledOnTouchOutside(!config.isCancelButtonDisabled)

        val header = stringFromChars(config.headerText)
        binding.header.text = header
        binding.header.visibility = if (header.isBlank()) View.GONE else View.VISIBLE
        val sub = stringFromChars(config.subText)
        binding.sub.text = sub
        binding.sub.visibility = if (sub.isBlank()) View.GONE else View.VISIBLE

        val multiline = config.inputFormMode == InputFormMode.MultiLine && config.isUseNewLine
        binding.textInput.inputType = when {
            config.keyboardMode == KeyboardMode.Numeric && config.passwordMode == PasswordMode.Hide ->
                InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD
            config.keyboardMode == KeyboardMode.Numeric -> InputType.TYPE_CLASS_NUMBER
            config.passwordMode == PasswordMode.Hide ->
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            config.keyboardMode == KeyboardMode.ASCII ->
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            else -> InputType.TYPE_CLASS_TEXT
        }.let { type -> if (multiline) type or InputType.TYPE_TEXT_FLAG_MULTI_LINE else type and InputType.TYPE_TEXT_FLAG_MULTI_LINE.inv() }
        binding.textInput.isSingleLine = !multiline
        binding.textInput.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or
            (if (multiline) EditorInfo.IME_ACTION_NONE else EditorInfo.IME_ACTION_DONE)

        val okText = stringFromChars(config.okText)
        if (okText.isNotBlank())
            binding.okButton.text = okText
        val guideText = stringFromChars(config.guideText)
        if (guideText.isNotBlank())
            binding.inputLayout.hint = guideText

        if (inline) {
            // Keep inline SWKBD lightweight: the game stays visible while Android's IME
            // edits a single visible text line. The EditText remains the InputConnection
            // host, but the full dialog chrome is hidden.
            binding.inputDialog.setBackgroundColor(Color.TRANSPARENT)
            binding.header.visibility = View.GONE
            binding.sub.visibility = View.GONE
            binding.inputLayout.hint = null
            binding.inputLayout.boxBackgroundMode = TextInputLayout.BOX_BACKGROUND_NONE
            binding.inputLayout.setBackgroundColor(Color.TRANSPARENT)
            binding.textInput.background = null
            binding.textInput.isCursorVisible = true
            binding.textInput.setPadding(0, binding.textInput.paddingTop, 0, binding.textInput.paddingBottom)
            binding.inlineUnderline.visibility = View.VISIBLE
            binding.lengthStatus.visibility = View.GONE
            binding.cancelButton.visibility = View.GONE
            binding.okButton.visibility = View.GONE
        }

        binding.textInput.filters = arrayOf(SoftwareKeyboardFilter(config))
        suppressTextEvent = true
        val text = pendingText ?: savedInstanceState?.getString(argumentInitialText) ?: initialText
        binding.textInput.setText(text)
        val initialCursor = if (config.initialCursorPos == InitialCursorPos.First) 0 else text.length
        binding.textInput.setSelection(initialCursor.coerceIn(0, text.length))
        suppressTextEvent = false

        binding.textInput.doOnTextChanged { value, _, _, _ ->
            val current = value ?: ""
            updateValidity(current)
            if (inline && !suppressTextEvent && !waitingForTextCheck)
                sendEvent(eventTextChanged, current.toString(), binding.textInput.selectionStart.coerceAtLeast(0))
        }
        updateValidity(text)

        binding.okButton.setOnClickListener { submit() }
        binding.cancelButton.visibility =
            if (inline || config.isCancelButtonDisabled) View.GONE else View.VISIBLE
        binding.cancelButton.setOnClickListener { cancelFromUser() }
        binding.textInput.setOnEditorActionListener { _, actionId, event ->
            val done = actionId == EditorInfo.IME_ACTION_DONE ||
                (!multiline && event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_UP)
            if (done)
                submit()
            done
        }
    }

    private fun updateValidity(text : CharSequence) {
        binding.okButton.isEnabled = !waitingForTextCheck && config.isValid(text)
        binding.lengthStatus.text = "${text.length}/${config.textMaxLength}"
    }

    override fun onStart() {
        super.onStart()
        if (::binding.isInitialized) {
            val window = dialog?.window
            if (inline) {
                window?.apply {
                    clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
                    setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
                    setDimAmount(0f)
                }
            }
            window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE)
            showImeWhenWindowIsFocused()
        }
    }

    private fun showImeWhenWindowIsFocused() {
        if (!::binding.isInitialized)
            return

        val window = dialog?.window ?: return
        val decorView = window.decorView

        fun showIme() {
            if (!isAdded || !::binding.isInitialized)
                return
            binding.textInput.requestFocus()
            binding.textInput.post {
                if (!isAdded || !::binding.isInitialized || !binding.textInput.hasFocus())
                    return@post
                val inputMethod = requireContext().getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
                inputMethod.showSoftInput(binding.textInput, InputMethodManager.SHOW_IMPLICIT)
            }
        }

        if (decorView.hasWindowFocus()) {
            showIme()
            return
        }

        imeWindowFocusListener?.let {
            if (decorView.viewTreeObserver.isAlive)
                decorView.viewTreeObserver.removeOnWindowFocusChangeListener(it)
        }

        val listener = object : ViewTreeObserver.OnWindowFocusChangeListener {
            override fun onWindowFocusChanged(hasFocus : Boolean) {
                if (!hasFocus)
                    return
                if (decorView.viewTreeObserver.isAlive)
                    decorView.viewTreeObserver.removeOnWindowFocusChangeListener(this)
                imeWindowFocusListener = null
                decorView.post { showIme() }
            }
        }
        imeWindowFocusListener = listener
        decorView.viewTreeObserver.addOnWindowFocusChangeListener(listener)
    }

    override fun onStop() {
        dialog?.window?.decorView?.let { decorView ->
            imeWindowFocusListener?.let { listener ->
                if (decorView.viewTreeObserver.isAlive)
                    decorView.viewTreeObserver.removeOnWindowFocusChangeListener(listener)
            }
        }
        imeWindowFocusListener = null
        super.onStop()
    }

    private fun submit() {
        if (waitingForTextCheck || !::binding.isInitialized || !config.isValid(binding.textInput.text ?: ""))
            return
        waitingForTextCheck = true
        setEditingEnabled(false)
        sendEvent(eventSubmit, binding.textInput.text.toString(), binding.textInput.selectionStart.coerceAtLeast(0))
    }

    private fun cancelFromUser() {
        if (config.isCancelButtonDisabled || terminalEventSent)
            return
        terminalEventSent = true
        sendEvent(eventCancel, currentText(), currentCursor())
    }

    private fun sendEvent(type : Int, text : String, cursor : Int) {
        (activity as? EmulationActivity)?.sendSoftwareKeyboardEvent(sessionId, type, text, cursor)
    }

    private fun currentText() = if (::binding.isInitialized) binding.textInput.text?.toString().orEmpty() else pendingText ?: initialText
    private fun currentCursor() = if (::binding.isInitialized) binding.textInput.selectionStart.coerceAtLeast(0) else 0

    private fun setEditingEnabled(enabled : Boolean) {
        if (!::binding.isInitialized)
            return
        binding.textInput.isEnabled = enabled
        binding.cancelButton.isEnabled = enabled
        updateValidity(binding.textInput.text ?: "")
    }

    fun updateFromFrontend(text : String, cursor : Int) {
        pendingText = text
        if (!::binding.isInitialized)
            return
        suppressTextEvent = true
        binding.textInput.setText(text)
        binding.textInput.setSelection(cursor.coerceIn(0, text.length))
        suppressTextEvent = false
        updateValidity(text)
    }

    fun resumeEditing() {
        waitingForTextCheck = false
        setEditingEnabled(true)
        if (::binding.isInitialized) {
            binding.textInput.requestFocus()
            binding.textInput.setSelection(binding.textInput.text?.length ?: 0)
        }
    }

    fun showTextCheck(result : Int, message : String) {
        if (!isAdded || closedByFrontend)
            return
        val confirm = result == textCheckConfirm
        var resolved = false
        fun resolve(accepted : Boolean) {
            if (resolved)
                return
            resolved = true
            sendEvent(if (accepted) eventTextCheckAccepted else eventTextCheckDismissed, currentText(), currentCursor())
        }
        MaterialAlertDialogBuilder(requireContext())
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

    fun closeFromFrontend() {
        closedByFrontend = true
        dismissAllowingStateLoss()
    }

    override fun onCancel(dialog : DialogInterface) {
        cancelFromUser()
        super.onCancel(dialog)
    }

    override fun onDismiss(dialog : DialogInterface) {
        if (!closedByFrontend && !terminalEventSent)
            cancelFromUser()
        (activity as? EmulationActivity)?.onSoftwareKeyboardDismissed(sessionId, this)
        super.onDismiss(dialog)
    }

    override fun onSaveInstanceState(outState : Bundle) {
        outState.putString(argumentInitialText, currentText())
        super.onSaveInstanceState(outState)
    }
}
