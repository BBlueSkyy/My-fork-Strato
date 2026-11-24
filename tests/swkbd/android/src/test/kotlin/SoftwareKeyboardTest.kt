// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato.applet.swkbd

import android.text.SpannableStringBuilder
import android.text.Spanned
import android.view.View
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(manifest = Config.NONE, sdk = [29])
class SoftwareKeyboardTest {
    private fun config() = SoftwareKeyboardConfig().apply { textMaxLength = 100u }
    private fun filter(source : CharSequence, start : Int = 0, end : Int = source.length,
                       config : SoftwareKeyboardConfig = config(), dest : String = "", dstart : Int = 0, dend : Int = dest.length) =
        SoftwareKeyboardFilter(config).filter(source, start, end, SpannableStringBuilder(dest), dstart, dend)!!

    @Test fun clippedComposingSpanDoesNotIndexOutsideSource() {
        val span = Any()
        val source = SpannableStringBuilder("abcd").apply { setSpan(span, 0, 4, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE) }
        val result = filter(source, 1, 3) as Spanned
        assertEquals("bc", result.toString())
        assertEquals(0, result.getSpanStart(span))
        assertEquals(2, result.getSpanEnd(span))
    }

    @Test fun emptySelectionAndZeroLengthSpansDoNotCrash() {
        val source = SpannableStringBuilder("abc").apply { setSpan(Any(), 1, 1, Spanned.SPAN_MARK_MARK) }
        assertEquals("", filter(source, 1, 1).toString())
    }

    @Test fun supplementaryCharacterKeepsWholeComposingSpan() {
        val span = Any()
        val source = SpannableStringBuilder("\uD83D\uDE00").apply { setSpan(span, 0, 2, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE) }
        val result = filter(source) as Spanned
        assertEquals(source.toString(), result.toString())
        assertEquals(2, result.getSpanEnd(span))
    }

    @Test fun spansRemainValidAfterCharactersAreRemovedOrLengthLimited() {
        val span = Any()
        val source = SpannableStringBuilder("a@bc").apply { setSpan(span, 0, 4, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE) }
        val config = config().apply { textMaxLength = 2u; invalidCharsFlags.atMark = true }
        val result = filter(source, config = config) as Spanned
        assertEquals("ab", result.toString())
        assertEquals(0, result.getSpanStart(span))
        assertEquals(2, result.getSpanEnd(span))
    }

    @Test fun replacementRespectsUtf16AndUtf8Limits() {
        assertEquals("", filter("\uD83D\uDE00", config = config().apply { textMaxLength = 1u }).toString())
        assertEquals("\uD83D\uDE00", filter("\uD83D\uDE00", config = config().apply { textMaxLength = 2u }).toString())
        val config = config().apply { isUseUtf8 = true; textMaxLength = 2004u }
        assertEquals("é".repeat(1002), filter("é".repeat(1003), config = config).toString())
        assertEquals("é", filter("é", config = config, dest = "a".repeat(2004), dstart = 0, dend = 2).toString())
    }

    @Test fun restrictionsAndDownloadCodeConversionArePreserved() {
        assertEquals("12", filter("1a2", config = config().apply { keyboardMode = KeyboardMode.Numeric }).toString())
        assertEquals("AB2", filter("ab2i", config = config().apply { invalidCharsFlags.outsideOfDownloadCode = true }).toString())
    }

    @Test fun controlsRestoreAllVisibilityStatesAcrossRepeatedSessions() {
        val views = listOf(View(RuntimeEnvironment.getApplication()), View(RuntimeEnvironment.getApplication()), View(RuntimeEnvironment.getApplication()))
        val original = listOf(View.VISIBLE, View.INVISIBLE, View.GONE)
        views.zip(original).forEach { (view, visibility) -> view.visibility = visibility }
        var releases = 0
        val controls = SoftwareKeyboardControls(views) { releases++ }
        repeat(3) {
            controls.update(true, false)
            controls.update(true, false) // same-session replacement must keep the original snapshot
            assertTrue(views.all { it.visibility == View.GONE })
            controls.update(false, false) // confirmation, cancellation, or failed opening
            assertEquals(original, views.map { it.visibility })
        }
        assertEquals(3, releases)
    }

    @Test fun pictureInPictureAndKeyboardCanCloseInEitherOrder() {
        val view = View(RuntimeEnvironment.getApplication())
        val controls = SoftwareKeyboardControls(listOf(view)) {}
        controls.update(true, false)
        controls.update(true, true)
        controls.update(false, true)
        assertEquals(View.GONE, view.visibility)
        controls.update(false, false)
        assertEquals(View.VISIBLE, view.visibility)
        controls.update(false, true)
        controls.update(true, true)
        controls.update(true, false)
        assertEquals(View.GONE, view.visibility)
        controls.update(false, false)
        assertEquals(View.VISIBLE, view.visibility)
    }
}
