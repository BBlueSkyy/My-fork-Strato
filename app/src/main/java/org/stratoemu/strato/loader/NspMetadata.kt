/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2025 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato.loader

/**
 * Data class containing metadata extracted from an NSP file
 */
data class NspMetadata(
    val name: String,
    val version: String,
    val titleId: String,
    val parentTitleId: String,
    val contentType: Int // 0=Unknown, 1=Base, 2=Update, 3=DLC
) {
    enum class ContentType(val value: Int) {
        Unknown(0),
        Base(1),
        Update(2),
        DLC(3);

        companion object {
            fun fromValue(value: Int) = values().firstOrNull { it.value == value } ?: Unknown
        }
    }

    val type: ContentType get() = ContentType.fromValue(contentType)
    
    val isBase: Boolean get() = contentType == 1
    val isUpdate: Boolean get() = contentType == 2
    val isDlc: Boolean get() = contentType == 3
}