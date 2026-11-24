// SPDX-License-Identifier: MPL-2.0
@file:OptIn(ExperimentalUnsignedTypes::class)
package org.stratoemu.strato.applet.swkbd

import android.graphics.Bitmap
import android.graphics.Canvas
import android.os.Bundle
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import androidx.appcompat.app.AppCompatActivity
import androidx.core.graphics.Insets
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.util.ReflectionHelpers
import org.stratoemu.strato.databinding.EmuActivityBinding
import org.stratoemu.strato.databinding.KeyboardDialogBinding
import org.stratoemu.strato.input.ControllerType
import java.nio.ByteBuffer

/** Minimal Activity hosting the production keyboard host, dialog, controller and XML. Native completions are recorded below. */
class KeyboardTestActivity : AppCompatActivity(), SoftwareKeyboardHostCallbacks {
    lateinit var binding : EmuActivityBinding
    lateinit var host : SoftwareKeyboardHost
    val events = mutableListOf<Pair<Long, Int>>()
    var buttons = 0
    var autoComplete = true
    override fun onCreate(state : Bundle?) {
        setTheme(com.google.android.material.R.style.Theme_Material3_DayNight_NoActionBar)
        super.onCreate(state)
        binding = EmuActivityBinding.inflate(layoutInflater)
        setContentView(binding.root)
        binding.perfStats.visibility = View.GONE
        binding.onScreenControllerView.controllerType = ControllerType.HandheldProController
        binding.onScreenControllerView.setOnButtonStateChangedListener { _, _ -> buttons++ }
        val controls = SoftwareKeyboardControls(listOf(binding.onScreenControlsContainer, binding.onScreenControllerView, binding.onScreenControllerToggle, binding.onScreenPauseToggle)) {
            binding.onScreenControllerView.releaseTouchInputs()
        }
        host = SoftwareKeyboardHost(this, controls) { id, event, _, _ ->
            events += id to event
            if ((event == SoftwareKeyboardDialog.eventSubmit && autoComplete) || event == SoftwareKeyboardDialog.eventCancel || event == SoftwareKeyboardDialog.eventTextCheckAccepted)
                host.close(id) // normal completion / inline hide response from the native applet
            if (event == SoftwareKeyboardDialog.eventTextCheckDismissed)
                host.resume(id)
        }
    }
    override fun sendSoftwareKeyboardEvent(sessionId : Long, dialog : SoftwareKeyboardDialog, type : Int, text : String, cursor : Int) =
        host.sendEvent(sessionId, dialog, type, text, cursor)
    override fun onSoftwareKeyboardShown(sessionId : Long, dialog : SoftwareKeyboardDialog) = host.onShown(sessionId, dialog)
    override fun dispatchTouchEvent(event : MotionEvent) : Boolean =
        if (::host.isInitialized && host.isActive) true else super.dispatchTouchEvent(event)
    override fun onSoftwareKeyboardDismissed(sessionId : Long, dialog : SoftwareKeyboardDialog) = host.onDismissed(sessionId, dialog)
    override fun onDestroy() { host.destroy(); super.onDestroy() }
}

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [31], qualifiers = "w1280dp-h720dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SoftwareKeyboardFlowTest {
    private fun open(activity : KeyboardTestActivity, inline : Boolean, id : Long = 1, cancelDisabled : Boolean = false) : SoftwareKeyboardDialog {
        val config = SoftwareKeyboardConfig().apply { textMaxLength = 14u; isCancelButtonDisabled = cancelDisabled }
        assertTrue(activity.host.open(id, config.writeToByteBuffer(ByteBuffer.allocate(0x4C8).order(java.nio.ByteOrder.LITTLE_ENDIAN)).apply { rewind() }, "Name", inline))
        idle(activity)
        return activity.supportFragmentManager.findFragmentByTag("software-keyboard-$id") as SoftwareKeyboardDialog
    }
    private fun idle(activity : KeyboardTestActivity) {
        shadowOf(Looper.getMainLooper()).idle()
        activity.supportFragmentManager.executePendingTransactions()
        activity.binding.root.measure(View.MeasureSpec.makeMeasureSpec(1280, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(720, View.MeasureSpec.EXACTLY))
        activity.binding.root.layout(0, 0, 1280, 720)
    }
    private fun pixels(activity : KeyboardTestActivity) : Int {
        idle(activity)
        val bitmap = Bitmap.createBitmap(1280, 720, Bitmap.Config.ARGB_8888)
        activity.binding.root.draw(Canvas(bitmap))
        val values = IntArray(1280 * 720); bitmap.getPixels(values, 0, 1280, 0, 0, 1280, 720)
        return values.count { it != android.graphics.Color.BLACK && it != android.graphics.Color.TRANSPARENT }
    }
    private fun input(dialog : SoftwareKeyboardDialog) = KeyboardDialogBinding.bind(dialog.requireView()).textInput
    private fun ime(dialog : SoftwareKeyboardDialog, visible : Boolean) {
        val decor = dialog.requireDialog().window!!.decorView
        val attachInfo = ReflectionHelpers.getField<Any>(decor, "mAttachInfo")
        ReflectionHelpers.setField(attachInfo, "mHasWindowFocus", true)
        assertTrue(decor.hasWindowFocus())
        assertTrue(dialog.isResumed)
        val insets = WindowInsetsCompat.Builder().setInsets(WindowInsetsCompat.Type.ime(), if (visible) Insets.of(0, 0, 0, 300) else Insets.NONE)
            .setVisible(WindowInsetsCompat.Type.ime(), visible).build()
        ViewCompat.dispatchApplyWindowInsets(dialog.requireView(), insets)
    }

    @Test fun normalAndInlineOpenHideTheDrawnControllerAndConfirmRestoresIt() {
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            assertTrue(pixels(activity) > 0)
            val dialog = open(activity, inline)
            assertSame(activity.binding.onScreenControllerView, activity.findViewById(org.stratoemu.strato.R.id.on_screen_controller_view))
            val notTouchable = dialog.requireDialog().window!!.attributes.flags and android.view.WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE != 0
            assertEquals(inline, notTouchable)
            assertTrue(activity.host.isActive)
            assertTrue(activity.events.contains(1L to SoftwareKeyboardDialog.eventFrontendOpened))
            assertEquals(0, pixels(activity))
            input(dialog).onEditorAction(EditorInfo.IME_ACTION_DONE)
            idle(activity)
            assertFalse(activity.host.isActive)
            assertTrue(pixels(activity) > 0)
            controller.pause().stop().destroy()
        }
    }

    @Test fun imeHidingDoesNotEndNormal() = assertImeHiding(false)
    @Test fun imeHidingDoesNotEndInline() = assertImeHiding(true)
    private fun assertImeHiding(inline : Boolean) {
        run {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val dialog = open(activity, inline)
            ime(dialog, true)
            ime(dialog, false) // Insets can change during IME/window transitions without Back.
            idle(activity)
            assertTrue("IME geometry must not terminate SWKBD (inline=$inline)", activity.host.isActive)
            assertFalse(activity.events.any { it.second == SoftwareKeyboardDialog.eventCancel })
            assertEquals(0, pixels(activity))
            controller.pause().stop().destroy()
        }
    }

    @Test fun sessionReplacementAndBackgroundKeepControlsHidden() {
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val old = open(activity, inline)
            val replacement = open(activity, inline)
            activity.host.onDismissed(1, old)
            assertNotSame(old, replacement)
            assertTrue(activity.host.isActive)
            assertEquals(0, pixels(activity))
            controller.pause().stop().start().resume().visible()
            assertTrue(activity.host.isActive)
            assertEquals(0, pixels(activity))
            replacement.requireDialog().cancel()
            idle(activity)
            assertFalse(activity.host.isActive)
            assertTrue(pixels(activity) > 0)
            controller.pause().stop().destroy()
        }
    }
    @Test fun realBackCancelsOnceAndDisabledCancelKeepsBothModesOpen() {
        for (inline in listOf(false, true)) for (disabled in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val dialog = open(activity, inline, cancelDisabled = disabled)
            val edit = input(dialog)
            val downTime = android.os.SystemClock.uptimeMillis()
            edit.dispatchKeyEventPreIme(KeyEvent(downTime, downTime, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0))
            edit.dispatchKeyEventPreIme(KeyEvent(downTime, downTime + 1, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, 0))
            idle(activity)
            assertEquals(disabled, activity.host.isActive)
            assertEquals(if (disabled) 0 else 1, activity.events.count { it.second == SoftwareKeyboardDialog.eventCancel })
            assertEquals(disabled, pixels(activity) == 0)
            controller.pause().stop().destroy()
        }
    }

    @Test fun visibleFragmentWithoutJniReopenStillOwnsTheOverlay() {
        // FragmentManager restoration does not call the Activity's JNI entrypoint again.
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val config = SoftwareKeyboardConfig().apply { textMaxLength = 14u }
            val dialog = SoftwareKeyboardDialog.newInstance(42, config, "Name", inline)
            dialog.showNow(activity.supportFragmentManager, "restored-keyboard")
            assertTrue(activity.host.isActive)
            assertEquals(0, pixels(activity))
            dialog.requireDialog().cancel()
            idle(activity)
            assertFalse(activity.host.isActive)
            assertTrue(pixels(activity) > 0)
            controller.pause().stop().destroy()
        }
    }

    @Test fun staleRestoredDialogCannotReplaceAnActiveSession() {
        val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
        val activity = controller.get()
        val current = open(activity, false)
        val stale = SoftwareKeyboardDialog.newInstance(42, SoftwareKeyboardConfig().apply { textMaxLength = 14u }, "Old", true)
        stale.showNow(activity.supportFragmentManager, "stale-keyboard")
        idle(activity)
        assertTrue(activity.host.isActive)
        assertTrue(current.requireDialog().isShowing)
        assertFalse(stale.isAdded)
        assertEquals(0, pixels(activity))
        current.requireDialog().cancel(); idle(activity)
        assertFalse(activity.host.isActive)
        controller.pause().stop().destroy()
    }

    @Test fun parentGatePreventsDrawingAndTouchEvenIfAChildIsMadeVisible() {
        val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
        val activity = controller.get()
        assertTrue(pixels(activity) > 0)
        val view = activity.binding.onScreenControllerView
        fun tap() {
            val location = IntArray(2); view.getLocationOnScreen(location)
            val x = location[0] + view.width * 0.81f
            val adjustedHeight = view.width / org.stratoemu.strato.input.onscreen.OnScreenButton.CONFIGURED_ASPECT_RATIO
            val y = location[1] + adjustedHeight * 0.73f + (view.height - adjustedHeight).coerceAtLeast(0f)
            for (action in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
                val event = MotionEvent.obtain(0, 0, action, x, y, 0)
                activity.dispatchTouchEvent(event); event.recycle()
            }
        }
        tap()
        assertTrue("test tap must reach the real virtual controller before SWKBD", activity.buttons > 0)
        val dialog = open(activity, false)
        val before = activity.buttons
        // Simulate another visibility writer. The displayed parent remains suppressed.
        view.visibility = View.VISIBLE
        assertEquals(0, pixels(activity))
        tap()
        assertEquals(before, activity.buttons)
        dialog.requireDialog().cancel(); idle(activity)
        assertEquals(View.VISIBLE, view.visibility)
        tap()
        assertTrue(activity.buttons > before)
        controller.pause().stop().destroy()
    }

    @Test fun userVisibilitySurvivesThreeOpenCloseCyclesAndFailedOpening() {
        val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
        val activity = controller.get()
        activity.binding.onScreenControllerView.visibility = View.INVISIBLE
        activity.binding.onScreenPauseToggle.visibility = View.GONE
        activity.binding.onScreenControllerToggle.visibility = View.VISIBLE
        val views = listOf(activity.binding.onScreenControlsContainer, activity.binding.onScreenControllerView,
                           activity.binding.onScreenControllerToggle, activity.binding.onScreenPauseToggle)
        val expected = views.map { it.visibility }
        repeat(3) {
            val dialog = open(activity, it % 2 == 0)
            assertEquals(0, pixels(activity))
            dialog.requireDialog().cancel(); idle(activity)
            assertEquals(expected, views.map { it.visibility })
        }
        controller.saveInstanceState(Bundle())
        val config = SoftwareKeyboardConfig().apply { textMaxLength = 14u }
        assertTrue(activity.host.open(99, config.writeToByteBuffer(ByteBuffer.allocate(0x4C8).order(java.nio.ByteOrder.LITTLE_ENDIAN)).apply { rewind() }, "Name", false))
        idle(activity)
        assertFalse(activity.host.isActive)
        assertTrue(activity.events.contains(99L to SoftwareKeyboardDialog.eventFrontendDestroyed))
        assertEquals(expected, views.map { it.visibility })
        controller.pause().stop().destroy()
    }

    @Test fun textCheckFocusAndRetryDoNotReleaseControls() {
        val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
        val activity = controller.get()
        activity.autoComplete = false
        val dialog = open(activity, false)
        input(dialog).onEditorAction(EditorInfo.IME_ACTION_DONE)
        activity.host.showTextCheck(1, SoftwareKeyboardDialog.textCheckFailure, "Please edit the name")
        idle(activity)
        assertTrue(activity.host.isActive)
        assertEquals(0, pixels(activity))
        val alert = org.robolectric.shadows.ShadowDialog.getLatestDialog() as androidx.appcompat.app.AlertDialog
        alert.getButton(android.content.DialogInterface.BUTTON_POSITIVE).performClick()
        idle(activity)
        assertTrue(input(dialog).isEnabled)
        assertTrue(activity.host.isActive)
        assertEquals(0, pixels(activity))
        dialog.requireDialog().cancel(); idle(activity)
        assertFalse(activity.host.isActive)
        assertTrue(pixels(activity) > 0)
        controller.pause().stop().destroy()
    }

}
