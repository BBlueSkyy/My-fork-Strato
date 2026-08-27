/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.preference

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.util.AttributeSet
import androidx.preference.Preference
import androidx.preference.Preference.SummaryProvider
import androidx.preference.PreferenceManager
import androidx.preference.R
import org.stratoemu.strato.GameFoldersActivity

class FolderPickerPreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = R.attr.preferenceStyle
) : Preference(context, attrs, defStyleAttr) {

    companion object {
        const val SEARCH_LOCATIONS_KEY = "search_locations"
    }

    private val prefs
        get() = PreferenceManager.getDefaultSharedPreferences(context)

    private val preferenceChangeListener =
        SharedPreferences.OnSharedPreferenceChangeListener { _, changedKey ->
            if (changedKey == SEARCH_LOCATIONS_KEY || changedKey == key) {
                notifyChanged()
            }
        }

    init {
        summaryProvider = SummaryProvider<FolderPickerPreference> { preference ->
            val count = preference.getSearchLocations().size

            preference.context.resources.getQuantityString(
                org.stratoemu.strato.R.plurals.game_folder_count,
                count,
                count
            )
        }
    }

    private fun getSearchLocations(): Set<String> {
        val locations = prefs
            .getStringSet(SEARCH_LOCATIONS_KEY, emptySet())
            ?.filter { it.isNotBlank() }
            ?.toMutableSet()
            ?: mutableSetOf()

        if (locations.isEmpty()) {
            val legacyLocation = prefs.getString(key, "")
            if (!legacyLocation.isNullOrBlank()) {
                locations.add(legacyLocation)

                prefs.edit()
                    .putStringSet(
                        SEARCH_LOCATIONS_KEY,
                        HashSet(locations)
                    )
                    .apply()
            }
        }

        return locations
    }

    override fun onAttached() {
        super.onAttached()
        prefs.registerOnSharedPreferenceChangeListener(
            preferenceChangeListener
        )
    }

    override fun onDetached() {
        prefs.unregisterOnSharedPreferenceChangeListener(
            preferenceChangeListener
        )
        super.onDetached()
    }

    override fun onClick() {
        context.startActivity(
            Intent(
                context,
                GameFoldersActivity::class.java
            )
        )
    }
}
