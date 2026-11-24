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
    val payloads = mutableListOf<Triple<Int, String, Int>>()
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
        host = SoftwareKeyboardHost(this) { id, event, text, cursor ->
            events += id to event
            payloads += Triple(event, text, cursor)
            if ((event == SoftwareKeyboardDialog.eventSubmit && autoComplete) || event == SoftwareKeyboardDialog.eventCancel || event == SoftwareKeyboardDialog.eventTextCheckAccepted)
                host.close(id) // normal completion / inline hide response from the native applet
            if (event == SoftwareKeyboardDialog.eventTextCheckDismissed)
                host.resume(id)
        }
    }
    override fun sendSoftwareKeyboardEvent(sessionId : Long, dialog : SoftwareKeyboardDialog, type : Int, text : String, cursor : Int) =
        host.sendEvent(sessionId, dialog, type, text, cursor)
    override fun onSoftwareKeyboardShown(sessionId : Long, dialog : SoftwareKeyboardDialog) = host.onShown(sessionId, dialog)
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

    @Test fun realInputConnectionPreservesCompositionUnicodeAndCursorInBothModes() {
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val dialog = open(activity, inline)
            val edit = input(dialog)
            val info = EditorInfo()
            val connection = edit.onCreateInputConnection(info)
            assertNotNull("The focused SWKBD editor must provide an InputConnection", connection)
            assertTrue(connection!!.setSelection(0, edit.length()))
            assertTrue(connection.setComposingText("Jo", 1))
            assertTrue(connection.finishComposingText())
            assertTrue(connection.commitText("ão😀", 1))
            assertEquals("João😀", edit.text.toString())
            assertEquals(6, edit.selectionStart) // Android cursors are UTF-16 offsets.
            assertTrue(connection.setSelection(2, 2))
            edit.viewTreeObserver.dispatchOnPreDraw()
            if (inline)
                assertTrue(activity.payloads.contains(Triple(SoftwareKeyboardDialog.eventCursorMoved, "João😀", 2)))
            assertTrue(connection.performEditorAction(EditorInfo.IME_ACTION_DONE))
            idle(activity)
            assertTrue(activity.payloads.contains(Triple(SoftwareKeyboardDialog.eventSubmit, "João😀", 2)))
            assertFalse(activity.host.isActive)
            controller.pause().stop().destroy()
        }
    }

    @Test fun replacementIgnoresStaleInputEventsAndKeepsCurrentEditorConnection() {
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val stale = open(activity, inline)
            val current = open(activity, inline)
            val count = activity.payloads.size
            activity.host.sendEvent(1, stale, SoftwareKeyboardDialog.eventSubmit, "stale", 0)
            assertEquals(count, activity.payloads.size)
            assertTrue(activity.host.isActive)
            val edit = input(current)
            assertTrue(edit.hasFocus())
            val connection = edit.onCreateInputConnection(EditorInfo())!!
            assertTrue(connection.setSelection(0, edit.length()))
            assertTrue(connection.commitText("Current", 1))
            assertTrue(connection.performEditorAction(EditorInfo.IME_ACTION_DONE))
            idle(activity)
            assertTrue(activity.payloads.contains(Triple(SoftwareKeyboardDialog.eventSubmit, "Current", 7)))
            assertFalse(activity.host.isActive)
            controller.pause().stop().destroy()
        }
    }
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

    @Test fun normalAndInlineOpenPreserveDrawnControlsAndConfirmation() {
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
            assertTrue(pixels(activity) > 0)
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
            assertTrue(pixels(activity) > 0)
            controller.pause().stop().destroy()
        }
    }

    @Test fun sessionReplacementAndBackgroundPreserveTouchVisibility() {
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val old = open(activity, inline)
            val replacement = open(activity, inline)
            activity.host.onDismissed(1, old)
            assertNotSame(old, replacement)
            assertTrue(activity.host.isActive)
            assertTrue(pixels(activity) > 0)
            controller.pause().stop().start().resume().visible()
            assertTrue(activity.host.isActive)
            assertTrue(pixels(activity) > 0)
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
            assertTrue(pixels(activity) > 0)
            controller.pause().stop().destroy()
        }
    }

    @Test fun visibleFragmentWithoutJniReopenStillRegistersItsSession() {
        // FragmentManager restoration does not call the Activity's JNI entrypoint again.
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val config = SoftwareKeyboardConfig().apply { textMaxLength = 14u }
            val dialog = SoftwareKeyboardDialog.newInstance(42, config, "Name", inline)
            dialog.showNow(activity.supportFragmentManager, "restored-keyboard")
            assertTrue(activity.host.isActive)
            assertTrue(pixels(activity) > 0)
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
        assertTrue(pixels(activity) > 0)
        current.requireDialog().cancel(); idle(activity)
        assertFalse(activity.host.isActive)
        controller.pause().stop().destroy()
    }

    @Test fun inlineKeyboardPreservesVirtualControllerTouchInput() {
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
        val dialog = open(activity, true)
        val before = activity.buttons
        assertTrue(pixels(activity) > 0)
        tap()
        assertTrue("inline input must keep the original controller touch path", activity.buttons > before)
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
        val views = listOf(activity.binding.onScreenControllerView,
                           activity.binding.onScreenControllerToggle, activity.binding.onScreenPauseToggle)
        val expected = views.map { it.visibility }
        repeat(3) {
            val dialog = open(activity, it % 2 == 0)
            assertEquals(expected, views.map { view -> view.visibility })
            assertTrue(pixels(activity) > 0)
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

    @Test fun textCheckFocusAndRetryPreserveTouchSettings() {
        val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
        val activity = controller.get()
        activity.autoComplete = false
        val dialog = open(activity, false)
        input(dialog).onEditorAction(EditorInfo.IME_ACTION_DONE)
        activity.host.showTextCheck(1, SoftwareKeyboardDialog.textCheckFailure, "Please edit the name")
        idle(activity)
        assertTrue(activity.host.isActive)
        assertTrue(pixels(activity) > 0)
        val alert = org.robolectric.shadows.ShadowDialog.getLatestDialog() as androidx.appcompat.app.AlertDialog
        assertEquals(java.util.Optional.of(false), navigationRequest())
        assertEquals(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE, alert.window!!.insetsController!!.systemBarsBehavior)
        alert.getButton(android.content.DialogInterface.BUTTON_POSITIVE).performClick()
        idle(activity)
        assertTrue(input(dialog).isEnabled)
        assertTrue(activity.host.isActive)
        assertTrue(pixels(activity) > 0)
        dialog.requireDialog().cancel(); idle(activity)
        assertFalse(activity.host.isActive)
        assertTrue(pixels(activity) > 0)
        controller.pause().stop().destroy()
    }
    @Test fun explicitlyHiddenUserControlsAreNeverForcedVisible() {
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val views = listOf(activity.binding.onScreenControllerView, activity.binding.onScreenControllerToggle, activity.binding.onScreenPauseToggle)
            views.forEach { it.visibility = View.GONE }
            val dialog = open(activity, inline)
            assertTrue(views.all { it.visibility == View.GONE })
            assertEquals(0, pixels(activity))
            dialog.requireDialog().cancel(); idle(activity)
            assertTrue(views.all { it.visibility == View.GONE })
            controller.pause().stop().destroy()
        }
    }

    private fun navigationRequest() = ReflectionHelpers.getStaticField<java.util.Optional<Boolean>>(
        org.robolectric.shadows.ShadowViewRootImpl::class.java, "isNavigationBarVisible")

    @Test fun navigationRequestTargetsTheDialogWithoutHidingStatusOrDisablingTextInput() {
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val navigationMode = activity.resources.getIdentifier("config_navBarInteractionMode", "integer", "android")
            assertTrue(navigationMode != 0)
            assertEquals("API 31 fixture must use three-button navigation", 0, activity.resources.getInteger(navigationMode))
            val activityBehavior = activity.window.insetsController!!.systemBarsBehavior
            val dialog = open(activity, inline)
            val window = dialog.requireDialog().window!!
            window.insetsController!!.show(android.view.WindowInsets.Type.statusBars() or android.view.WindowInsets.Type.navigationBars())
            SoftwareKeyboardWindow.hideNavigationBar(window)
            assertEquals(java.util.Optional.of(false), navigationRequest())
            val statusRequest = ReflectionHelpers.getStaticField<java.util.Optional<Boolean>>(
                org.robolectric.shadows.ShadowViewRootImpl::class.java, "isStatusBarVisible")
            assertEquals(java.util.Optional.of(true), statusRequest)
            assertEquals(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE, window.insetsController!!.systemBarsBehavior)
            assertEquals(activityBehavior, activity.window.insetsController!!.systemBarsBehavior)
            val incompatibleFlags = android.view.WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or android.view.WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM
            assertEquals(0, window.attributes.flags and incompatibleFlags)
            assertTrue(input(dialog).isEnabled)
            assertTrue(input(dialog).hasFocus())
            input(dialog).setText("Edited")
            assertEquals("Edited", input(dialog).text.toString())
            input(dialog).onEditorAction(EditorInfo.IME_ACTION_DONE); idle(activity)
            assertFalse(activity.host.isActive)
            controller.pause().stop().destroy()
        }
    }

    @Test fun dialogReappliesNavigationRequestWhenItRegainsFocus() {
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val dialog = open(activity, inline)
            dialog.requireDialog().onWindowFocusChanged(false)
            dialog.requireDialog().window!!.insetsController!!.show(android.view.WindowInsets.Type.navigationBars())
            assertEquals(java.util.Optional.of(true), navigationRequest())
            dialog.requireDialog().onWindowFocusChanged(true)
            assertEquals(java.util.Optional.of(false), navigationRequest())
            assertTrue(activity.host.isActive)
            assertTrue(input(dialog).isEnabled)
            controller.pause().stop().destroy()
        }
    }

    @Test fun systemControlledNavigationVisibilityDoesNotCauseLoopsOrCancelKeyboard() {
        for (inline in listOf(false, true)) {
            val controller = Robolectric.buildActivity(KeyboardTestActivity::class.java).setup()
            val activity = controller.get()
            val dialog = open(activity, inline)
            // Robolectric does not run system_server. Emulate a system-owned visible bar;
            // the client must not treat that visibility or IME insets as a close/retry signal.
            dialog.requireDialog().window!!.insetsController!!.show(android.view.WindowInsets.Type.navigationBars())
            ime(dialog, true)
            idle(activity)
            assertEquals(java.util.Optional.of(true), navigationRequest())
            assertTrue(activity.host.isActive)
            assertFalse(activity.events.any { it.second == SoftwareKeyboardDialog.eventCancel })
            assertTrue(input(dialog).isEnabled)
            assertTrue(pixels(activity) > 0)
            controller.pause().stop().destroy()
        }
    }


}
