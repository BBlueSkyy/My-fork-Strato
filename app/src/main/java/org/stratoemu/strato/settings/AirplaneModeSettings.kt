/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2026 Strato Contributors
 */

package org.stratoemu.strato.settings

import android.content.Context
import androidx.preference.PreferenceManager

/**
 * Stores the user's emulated Airplane Mode directly.
 *
 * The old "is_internet_enabled" preference is migrated once for each preference
 * store (global and per-game). Neither the native JSON field nor the Horizon
 * service ABI changes as a result of this UI rename.
 */
object AirplaneModeSettings {
    const val KEY = "is_airplane_mode"
    private const val LEGACY_KEY = "is_internet_enabled"

    fun migrate(context: Context, prefName: String? = null) {
        val preferences = prefName?.let { context.getSharedPreferences(it, Context.MODE_PRIVATE) }
            ?: PreferenceManager.getDefaultSharedPreferences(context)

        // Repeated launches must never overwrite an explicit Airplane Mode choice.
        val values = preferences.all
        if (values[KEY] is Boolean) return

        // Preserve the actual pre-upgrade network state (and the offline default).
        val oldInternetEnabled = values[LEGACY_KEY] as? Boolean ?: false
        preferences.edit().putBoolean(KEY, AirplaneModePolicy.fromLegacyInternetEnabled(oldInternetEnabled)).apply()
    }
}
