/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2026 Strato Contributors
 */

package org.stratoemu.strato.preference

import android.content.Context
import android.util.AttributeSet
import androidx.preference.R
import androidx.preference.SwitchPreferenceCompat

/**
 * Presents the existing is_internet_enabled setting as its inverse: Airplane Mode.
 *
 * Using the unchanged preference key preserves existing global and per-game values,
 * the native settings ABI and the user's network permission ceiling. The emulated
 * NIFM/set:sys state remains managed separately by the service layer.
 */
class AirplaneModePreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = R.attr.switchPreferenceCompatStyle
) : SwitchPreferenceCompat(context, attrs, defStyleAttr) {

    override fun onSetInitialValue(defaultValue: Any?) {
        val airplaneModeDefault = defaultValue as? Boolean ?: true
        // Read the old "internet enabled" bit and invert it for the UI.
        isChecked = !getPersistedBoolean(!airplaneModeDefault)
    }

    override fun persistBoolean(value: Boolean): Boolean {
        // The superclass reads the raw preference, so only invert on write.
        return super.persistBoolean(!value)
    }
}
