/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2026 Strato Contributors
 */
package org.stratoemu.strato.preference

import android.content.SharedPreferences
import androidx.preference.SwitchPreferenceCompat

/**
 * Present the proven is_internet_enabled preference as Airplane Mode.
 *
 * No new persisted keys, migrations or changes to EmulationSettings,
 * NativeSettings and their native serialization.
 */
object AirplaneModePreferenceBinder {
    private const val INTERNET_KEY = "is_internet_enabled"

    fun bind(preference: SwitchPreferenceCompat?, preferences: SharedPreferences?) {
        if (preference == null || preferences == null) return

        preference.isPersistent = false
        val internetEnabled = try {
            preferences.getBoolean(INTERNET_KEY, false)
        } catch (_: ClassCastException) {
            false
        }
        preference.isChecked = !internetEnabled

        preference.setOnPreferenceChangeListener { _, value ->
            val airplaneMode = value as? Boolean ?: return@setOnPreferenceChangeListener false
            // SharedPreferences.apply() updates in-memory values immediately;
            // NativeSettings reads this same key on the next game launch.
            preferences.edit().putBoolean(INTERNET_KEY, !airplaneMode).apply()
            true
        }
    }
}
