/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2026 Strato Contributors
 */

package org.stratoemu.strato.settings

/**
 * The UI stores "airplane mode" (enabled means offline); libskyline's native
 * settings ABI continues to consume "isInternetEnabled" (enabled means online).
 *
 * Keep these conversions in one Android-independent place for host regressions.
 */
object AirplaneModePolicy {
    fun fromLegacyInternetEnabled(legacyInternetEnabled: Boolean): Boolean = !legacyInternetEnabled
    fun toNativeInternetEnabled(isAirplaneMode: Boolean): Boolean = !isAirplaneMode
}
