/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.applet.swkbd

import android.text.InputFilter
import android.text.SpannableStringBuilder
import android.text.Spanned

class SoftwareKeyboardFilter(val config : SoftwareKeyboardConfig) : InputFilter {
    override fun filter(source : CharSequence, start : Int, end : Int, dest : Spanned, dstart : Int, dend : Int) : CharSequence? {
        val filteredStringBuilder = SpannableStringBuilder()
        // Map UTF-16 boundaries, including the end boundary and surrogate pairs.
        val boundaries = IntArray(end - start + 1)

        val sourceCodepoints = source.subSequence(start, end).codePoints().iterator()
        val replacedLength = dest.subSequence(dstart, dend).length
        var encodedLength = if (config.isUseUtf8) {
            dest.toString().toByteArray(Charsets.UTF_8).size -
                dest.subSequence(dstart, dend).toString().toByteArray(Charsets.UTF_8).size
        } else 0

        var nextIndex = start
        while (sourceCodepoints.hasNext()) {
            var codepoint = sourceCodepoints.next()
            val currentIndex = nextIndex
            nextIndex += Character.charCount(codepoint)
            for (index in currentIndex until nextIndex)
                boundaries[index - start] = filteredStringBuilder.length

            if (config.invalidCharsFlags.outsideOfDownloadCode)
                codepoint = Character.toUpperCase(codepoint)
            boundaries[nextIndex - start] = filteredStringBuilder.length
            if (!config.isValid(codepoint))
                continue
            // Check if the string would be over the maximum length after adding this character
            if (dest.length - replacedLength + filteredStringBuilder.length + Character.charCount(codepoint) > config.textMaxLength.toInt())
                break
            val character = String(Character.toChars(codepoint))
            if (config.isUseUtf8) {
                val encodedCharacterLength = character.toByteArray(Charsets.UTF_8).size
                if (encodedLength + encodedCharacterLength > SoftwareKeyboardConfigTextBytes)
                    break
                encodedLength += encodedCharacterLength
            }

            filteredStringBuilder.append(character)
            boundaries[nextIndex - start] = filteredStringBuilder.length
        }

        // Unprocessed characters (length limit) map to the end of the accepted text.
        for (index in nextIndex..end)
            boundaries[index - start] = filteredStringBuilder.length

        if (source is Spanned) {
            for (span in source.getSpans(start, end, Any::class.java)) {
                val spanStart = source.getSpanStart(span).coerceIn(start, end) - start
                val spanEnd = source.getSpanEnd(span).coerceIn(start, end) - start
                val newStart = boundaries[spanStart]
                val newEnd = boundaries[spanEnd]
                if (newStart < newEnd || source.getSpanFlags(span) and Spanned.SPAN_POINT_MARK_MASK != Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                    filteredStringBuilder.setSpan(span, newStart, newEnd, source.getSpanFlags(span))
            }
        }
        return filteredStringBuilder
    }
}
