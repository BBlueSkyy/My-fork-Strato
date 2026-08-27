/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.preference

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.AttributeSet
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.preference.Preference
import androidx.preference.Preference.SummaryProvider
import androidx.preference.PreferenceManager
import androidx.preference.R
import org.stratoemu.strato.di.getSettings

class FolderPickerPreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = R.attr.preferenceStyle
) : Preference(context, attrs, defStyleAttr) {

    companion object {
        const val SEARCH_LOCATIONS_KEY = "search_locations"
    }

    private val documentPicker =
        (context as ComponentActivity).registerForActivityResult(
            ActivityResultContracts.OpenDocumentTree()
        ) { uri ->
            uri?.let {
                try {
                    context.contentResolver.takePersistableUriPermission(
                        it,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION
                    )
                } catch (_: SecurityException) {
                }

                val locations = getSearchLocations().toMutableSet()
                locations.add(it.toString())

                val prefs = PreferenceManager.getDefaultSharedPreferences(context)
                val editor = prefs.edit()
                    .putStringSet(SEARCH_LOCATIONS_KEY, locations)

                val legacyLocation = prefs.getString(key, "")
                if (legacyLocation.isNullOrEmpty()) {
                    editor.putString(key, it.toString())
                }

                editor.apply()

                context.getSettings().refreshRequired = true
                notifyChanged()
            }
        }

    init {
        summaryProvider = SummaryProvider<FolderPickerPreference> { preference ->
            preference.getSearchLocations()
                .sorted()
                .joinToString("\n") { Uri.decode(it) }
        }
    }

    private fun getSearchLocations(): Set<String> {
        val prefs = PreferenceManager.getDefaultSharedPreferences(context)
        val locations = prefs.getStringSet(SEARCH_LOCATIONS_KEY, emptySet())
            ?.filter { it.isNotBlank() }
            ?.toMutableSet()
            ?: mutableSetOf()

        if (locations.isEmpty()) {
            val legacyLocation = prefs.getString(key, "")
            if (!legacyLocation.isNullOrBlank()) {
                locations.add(legacyLocation)
                prefs.edit()
                    .putStringSet(SEARCH_LOCATIONS_KEY, locations)
                    .apply()
            }
        }

        return locations
    }

    override fun onClick() = documentPicker.launch(null)
}
