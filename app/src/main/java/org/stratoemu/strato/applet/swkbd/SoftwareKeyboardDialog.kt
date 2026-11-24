/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.applet.swkbd

import android.annotation.SuppressLint
import android.app.Dialog
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
import androidx.activity.ComponentDialog
import androidx.core.widget.doOnTextChanged
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.fragment.app.DialogFragment
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.stratoemu.strato.databinding.KeyboardDialogBinding
import org.stratoemu.strato.utils.parcelable
import org.stratoemu.strato.utils.stringFromChars

class SoftwareKeyboardDialog : DialogFragment() {
    private val sessionId by lazy { requireArguments().getLong(argumentSessionId) }
    private val config by lazy { requireArguments().parcelable<SoftwareKeyboardConfig>(argumentConfig)!! }
    private val initialText by lazy { requireArguments().getString(argumentInitialText)!! }
    private val inline by lazy { requireArguments().getBoolean(argumentInline) }

    private var viewBinding : KeyboardDialogBinding? = null
    private val binding get() = viewBinding!!
    private var pendingText : String? = null
    private var pendingCursor : Int? = null
    private var textCheckDialog : androidx.appcompat.app.AlertDialog? = null
    private var pendingTextCheck : Pair<Int, String>? = null
    private var closedByFrontend = false
    private var terminalEventSent = false
    private var waitingForTextCheck = false
    private var suppressTextEvent = false
    private var observedText = ""
    private var observedCursor = 0
    private var cursorObserver : ViewTreeObserver.OnPreDrawListener? = null

    companion object {
        private const val argumentSessionId = "sessionId"
        private const val argumentConfig = "config"
        private const val argumentInitialText = "initialText"
        private const val argumentInline = "inline"
        private const val stateCursor = "cursor"
        private const val stateWaitingForTextCheck = "waitingForTextCheck"
        private const val stateTextCheckResult = "textCheckResult"
        private const val stateTextCheckMessage = "textCheckMessage"

        const val textCheckFailure = 1
        const val textCheckConfirm = 2

        const val eventTextChanged = 0
        const val eventSubmit = 1
        const val eventCancel = 2
        const val eventTextCheckAccepted = 3
        const val eventTextCheckDismissed = 4
        const val eventFrontendDestroyed = 5
        const val eventFrontendOpened = 6
        const val eventCursorMoved = 7

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

    override fun onCreateDialog(savedInstanceState : Bundle?) : Dialog = object : ComponentDialog(requireContext(), theme) {
        override fun onWindowFocusChanged(hasFocus : Boolean) {
            super.onWindowFocusChanged(hasFocus)
            if (hasFocus)
                window?.let(SoftwareKeyboardWindow::hideNavigationBar)
        }
    }

    override fun onCreateView(inflater : LayoutInflater, container : ViewGroup?, savedInstanceState : Bundle?) =
        KeyboardDialogBinding.inflate(inflater, container, false).also { viewBinding = it }.root

    @SuppressLint("SetTextI18n")
    override fun onViewCreated(view : View, savedInstanceState : Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        isCancelable = !config.isCancelButtonDisabled
        dialog?.setCanceledOnTouchOutside(!inline && !config.isCancelButtonDisabled)

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

        binding.textInput.filters = arrayOf(SoftwareKeyboardFilter(config))
        suppressTextEvent = true
        waitingForTextCheck = savedInstanceState?.getBoolean(stateWaitingForTextCheck) ?: waitingForTextCheck
        savedInstanceState?.getString(stateTextCheckMessage)?.let { message ->
            pendingTextCheck = savedInstanceState.getInt(stateTextCheckResult) to message
        }
        val text = pendingText ?: savedInstanceState?.getString(argumentInitialText) ?: initialText
        binding.textInput.setText(text)
        val length = binding.textInput.text?.length ?: 0
        val initialCursor = pendingCursor ?: savedInstanceState?.getInt(stateCursor)
            ?: if (config.initialCursorPos == InitialCursorPos.First) 0 else length
        binding.textInput.setSelection(initialCursor.coerceIn(0, length))
        suppressTextEvent = false

        binding.textInput.doOnTextChanged { value, _, _, _ ->
            val current = value ?: ""
            updateValidity(current)
            if (inline && !suppressTextEvent && !waitingForTextCheck) {
                sendEvent(eventTextChanged, current.toString(), binding.textInput.selectionStart.coerceAtLeast(0))
                observedText = current.toString()
                observedCursor = currentCursor()
            }
        }
        setEditingEnabled(!waitingForTextCheck)
        observedText = currentText()
        observedCursor = currentCursor()
        cursorObserver = ViewTreeObserver.OnPreDrawListener {
            val current = currentText()
            val cursor = currentCursor()
            if (inline && !suppressTextEvent && !waitingForTextCheck && current == observedText && cursor != observedCursor)
                sendEvent(eventCursorMoved, current, cursor)
            observedText = current
            observedCursor = cursor
            true
        }
        binding.textInput.viewTreeObserver.addOnPreDrawListener(cursorObserver)

        binding.okButton.setOnClickListener { submit() }
        if (inline) {
            // Preserve the working dialog geometry and focused TextInputEditText so Android
            // keeps a valid InputConnection. Inline mode only hides the frontend chrome;
            // the guest renders the actual text through its indirect layer.
            binding.inputDialog.setBackgroundColor(Color.TRANSPARENT)
            binding.header.visibility = View.INVISIBLE
            binding.sub.visibility = View.INVISIBLE
            binding.okButton.visibility = View.INVISIBLE
            binding.cancelButton.visibility = View.INVISIBLE
            binding.lengthStatus.visibility = View.INVISIBLE
            binding.inputLayout.hint = null
            binding.inputLayout.boxStrokeWidth = 0
            binding.inputLayout.boxStrokeWidthFocused = 0
            binding.inputLayout.setBoxBackgroundColor(Color.TRANSPARENT)
            binding.textInput.background = null
            binding.textInput.isCursorVisible = false
            binding.textInput.setTextColor(Color.TRANSPARENT)
            binding.textInput.setHintTextColor(Color.TRANSPARENT)
        } else {
            binding.okButton.visibility = View.VISIBLE
            binding.cancelButton.visibility = if (config.isCancelButtonDisabled) View.GONE else View.VISIBLE
            binding.lengthStatus.visibility = View.VISIBLE
        }
        binding.cancelButton.setOnClickListener { cancelFromUser() }
        binding.textInput.onImeBackPressed = ::cancelFromUser
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
        if (viewBinding != null) {
            dialog?.window?.apply {
                if (inline) {
                    clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
                    // Retain the IME connection and the original touch pass-through
                    // to the game and its on-screen controls.
                    addFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE)
                    setDimAmount(0f)
                    setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
                }
                setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE)
                SoftwareKeyboardWindow.hideNavigationBar(this)
            }
            binding.textInput.requestFocus()
            if (!closedByFrontend)
                (activity as? SoftwareKeyboardHostCallbacks)?.onSoftwareKeyboardShown(sessionId, this)
        }
    }

    private fun submit() {
        if (waitingForTextCheck || viewBinding == null || !config.isValid(binding.textInput.text ?: ""))
            return
        waitingForTextCheck = true
        setEditingEnabled(false)
        sendEvent(eventSubmit, binding.textInput.text.toString(), binding.textInput.selectionStart.coerceAtLeast(0))
    }

    override fun onResume() {
        super.onResume()
        pendingTextCheck?.let { (result, message) -> showTextCheck(result, message) }
        if (!waitingForTextCheck && !closedByFrontend && !terminalEventSent) {
            binding.textInput.requestFocus()
            dialog?.window?.let {
                WindowInsetsControllerCompat(it, binding.textInput).show(WindowInsetsCompat.Type.ime())
                SoftwareKeyboardWindow.hideNavigationBar(it)
            }
        }
    }

    private fun cancelFromUser() {
        if (config.isCancelButtonDisabled || terminalEventSent)
            return
        terminalEventSent = true
        sendEvent(eventCancel, currentText(), currentCursor())
    }

    private fun sendEvent(type : Int, text : String, cursor : Int) {
        if (!closedByFrontend && (!terminalEventSent || type == eventCancel || type == eventFrontendDestroyed))
            (activity as? SoftwareKeyboardHostCallbacks)?.sendSoftwareKeyboardEvent(sessionId, this, type, text, cursor)
    }

    private fun currentText() = if (viewBinding != null) binding.textInput.text?.toString().orEmpty() else pendingText ?: initialText
    private fun currentCursor() = if (viewBinding != null) binding.textInput.selectionStart.coerceAtLeast(0) else pendingCursor ?: 0

    private fun setEditingEnabled(enabled : Boolean) {
        if (viewBinding == null)
            return
        binding.textInput.isEnabled = enabled
        binding.cancelButton.isEnabled = enabled
        updateValidity(binding.textInput.text ?: "")
    }

    fun updateFromFrontend(text : String, cursor : Int) {
        pendingText = text
        pendingCursor = cursor
        if (viewBinding == null)
            return
        suppressTextEvent = true
        binding.textInput.setText(text)
        binding.textInput.setSelection(cursor.coerceIn(0, binding.textInput.text?.length ?: 0))
        observedText = currentText()
        observedCursor = currentCursor()
        suppressTextEvent = false
        updateValidity(binding.textInput.text ?: "")
    }

    fun resumeEditing() {
        waitingForTextCheck = false
        setEditingEnabled(true)
        if (viewBinding != null) {
            binding.textInput.requestFocus()
            dialog?.window?.let {
                WindowInsetsControllerCompat(it, binding.textInput).show(WindowInsetsCompat.Type.ime())
                SoftwareKeyboardWindow.hideNavigationBar(it)
            }
        }
    }

    fun showTextCheck(result : Int, message : String) {
        if (closedByFrontend)
            return
        pendingTextCheck = result to message
        if (!isAdded || viewBinding == null || !isResumed || textCheckDialog?.isShowing == true)
            return
        val confirm = result == textCheckConfirm
        var resolved = false
        fun resolve(accepted : Boolean) {
            if (resolved || closedByFrontend || viewBinding == null)
                return
            resolved = true
            pendingTextCheck = null
            sendEvent(if (accepted) eventTextCheckAccepted else eventTextCheckDismissed, currentText(), currentCursor())
        }
        textCheckDialog = MaterialAlertDialogBuilder(requireContext())
            .setMessage(message)
            .setPositiveButton(android.R.string.ok) { _, _ -> resolve(confirm) }
            .apply {
                if (confirm)
                    setNegativeButton(android.R.string.cancel) { _, _ -> resolve(false) }
                setOnCancelListener { resolve(false) }
                setOnDismissListener { resolve(false) }
            }
            .show().also { alert -> alert.window?.let(SoftwareKeyboardWindow::hideNavigationBar) }
    }

    fun closeFromFrontend() {
        closedByFrontend = true
        textCheckDialog?.dismiss()
        textCheckDialog = null
        dismissAllowingStateLoss()
    }

    override fun onCancel(dialog : DialogInterface) {
        cancelFromUser()
        super.onCancel(dialog)
    }

    override fun onDismiss(dialog : DialogInterface) {
        if (!closedByFrontend && !terminalEventSent) {
            if (config.isCancelButtonDisabled) {
                terminalEventSent = true
                sendEvent(eventFrontendDestroyed, currentText(), currentCursor())
            } else {
                cancelFromUser()
            }
        }
        (activity as? SoftwareKeyboardHostCallbacks)?.onSoftwareKeyboardDismissed(sessionId, this)
        super.onDismiss(dialog)
    }

    override fun onDestroyView() {
        pendingText = currentText()
        pendingCursor = currentCursor()
        cursorObserver?.let { binding.textInput.viewTreeObserver.removeOnPreDrawListener(it) }
        cursorObserver = null
        viewBinding?.textInput?.onImeBackPressed = null
        viewBinding = null
        textCheckDialog?.dismiss()
        textCheckDialog = null
        super.onDestroyView()
    }

    override fun onSaveInstanceState(outState : Bundle) {
        outState.putString(argumentInitialText, currentText())
        outState.putInt(stateCursor, currentCursor())
        outState.putBoolean(stateWaitingForTextCheck, waitingForTextCheck)
        pendingTextCheck?.let { (result, message) ->
            outState.putInt(stateTextCheckResult, result)
            outState.putString(stateTextCheckMessage, message)
        }
        super.onSaveInstanceState(outState)
    }
}
