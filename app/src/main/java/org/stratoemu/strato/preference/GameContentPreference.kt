/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2025 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato.preference

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import android.util.AttributeSet
import androidx.preference.Preference
import androidx.preference.PreferenceManager
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.stratoemu.strato.R
import org.stratoemu.strato.fragments.IndeterminateProgressDialogFragment
import org.stratoemu.strato.loader.NspParser
import org.stratoemu.strato.settings.SettingsActivity
import org.stratoemu.strato.utils.NspFilePicker
import org.stratoemu.strato.utils.serializable
import java.io.IOException

class GameContentPreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = androidx.preference.R.attr.preferenceStyle
) : Preference(context, attrs, defStyleAttr) {

    private var nspFilePicker: NspFilePicker? = null
    private val prefs: SharedPreferences = PreferenceManager.getDefaultSharedPreferences(context)
    
    // Game content storage keys - these should be based on the base game's title ID
    private var baseTitleId: String? = null
    
    companion object {
        private fun getUpdateKey(titleId: String) = "selected_update_$titleId"
        private fun getDlcListKey(titleId: String) = "selected_dlc_list_$titleId"
        private fun getUpdateVersionKey(titleId: String) = "update_version_$titleId"
        private fun getDlcNamesKey(titleId: String) = "dlc_names_$titleId"
    }

    init {
        // No default summary
        summary = ""
    }
    
    override fun onAttached() {
        super.onAttached()
        // Initialize NspFilePicker when the preference is attached to avoid lifecycle issues
        val activity = (context as? SettingsActivity) ?: return
        nspFilePicker?.cleanup()
        nspFilePicker = NspFilePicker.with(activity, activity.binding.root) { uri, fileName ->
            handleNspFilePicked(uri, fileName)
        }
    }

    fun setBaseTitleId(titleId: String) {
        this.baseTitleId = titleId
        updateSummary()
    }

    private fun updateSummary() {
        try {
            // Ne jamais afficher de description, même avec une mise à jour ou des DLC sélectionnés
            summary = ""
        } catch (e: Exception) {
            summary = ""
        }
    }

    override fun onClick() {
        // Use the NspFilePicker that was initialized in onAttached()
        nspFilePicker?.openNspFilePicker() ?: run {
            // If for some reason nspFilePicker is null, try to reinitialize
            val activity = (context as? SettingsActivity) ?: return
            nspFilePicker = NspFilePicker.with(activity, activity.binding.root) { uri, fileName ->
                handleNspFilePicked(uri, fileName)
            }
            nspFilePicker?.openNspFilePicker()
        }
    }
    
    override fun onDetached() {
        super.onDetached()
        // Clean up when the preference is detached
        nspFilePicker?.cleanup()
        nspFilePicker = null
    }

    private fun handleNspFilePicked(uri: Uri, fileName: String) {
        val task: () -> Unit = {
            try {
                // Parse NSP metadata to determine type
                val metadata = NspParser.parseNspMetadata(context, uri)
                
                metadata?.let { meta ->
                    // Detect base titleId from the NSP metadata if not already set
                    val targetTitleId = baseTitleId ?: extractBaseTitleId(meta)
                    
                    if (targetTitleId == null) {
                        CoroutineScope(Dispatchers.Main).launch {
                            showErrorMessage(R.string.error_no_base_game)
                        }
                        return@let
                    }
                    
                    // Set the detected titleId if we didn't have one
                    if (baseTitleId == null) {
                        baseTitleId = targetTitleId
                    }
                    
                    when {
                        meta.isUpdate -> {
                            // Validate it's an update for this game
                            if (NspParser.isValidUpdate(context, uri, targetTitleId)) {
                                // Take persistent URI permissions first
                                try {
                                    context.contentResolver.takePersistableUriPermission(uri, 
                                        android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                } catch (e: Exception) {
                                }
                                
                                // Add to ContentManager (new system) and enable it
                                org.stratoemu.strato.ContentManager.addUpdate(targetTitleId, uri, meta.name, meta.version)
                                org.stratoemu.strato.ContentManager.setUpdateEnabled(targetTitleId, uri, true)
                                
                                // Also save to old GameContentPreference for compatibility
                                saveSelectedUpdate(uri, meta.version)
                                
                                CoroutineScope(Dispatchers.Main).launch {
                                    updateSummary()
                                    showSuccessMessage(context.getString(R.string.update_selected_success, meta.version))
                                    
                                    // Open ManageContentActivity to show the added content
                                    openManageContentActivity()
                                }
                            } else {
                                CoroutineScope(Dispatchers.Main).launch {
                                    showErrorMessage(R.string.error_invalid_update)
                                }
                            }
                        }
                        
                        meta.isDlc -> {
                            // Validate it's DLC for this game
                            if (NspParser.isValidDlc(context, uri, targetTitleId)) {
                                // Take persistent URI permissions first
                                try {
                                    context.contentResolver.takePersistableUriPermission(uri, 
                                        android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                } catch (e: Exception) {
                                }
                                
                                // Add to ContentManager (new system) and enable it
                                org.stratoemu.strato.ContentManager.addDlc(targetTitleId, uri, meta.name)
                                org.stratoemu.strato.ContentManager.setDlcEnabled(targetTitleId, uri, true)
                                
                                // Also save to old GameContentPreference for compatibility
                                addSelectedDlc(uri, meta.name)
                                
                                CoroutineScope(Dispatchers.Main).launch {
                                    updateSummary()
                                    showSuccessMessage(context.getString(R.string.dlc_selected_success, meta.name))
                                    
                                    // Open ManageContentActivity to show the added content
                                    openManageContentActivity()
                                }
                            } else {
                                CoroutineScope(Dispatchers.Main).launch {
                                    showErrorMessage(R.string.error_invalid_dlc)
                                }
                            }
                        }
                        
                        else -> {
                            CoroutineScope(Dispatchers.Main).launch {
                                showErrorMessage(R.string.error_unsupported_content_type)
                            }
                        }
                    }
                } ?: run {
                    CoroutineScope(Dispatchers.Main).launch {
                        showErrorMessage(R.string.error_parsing_nsp)
                    }
                }
                
            } catch (e: Exception) {
                CoroutineScope(Dispatchers.Main).launch {
                    showErrorMessage(R.string.error_processing_nsp)
                }
            }
        }

        // Show a loading dialog while processing the NSP file
        (context as? SettingsActivity)?.let { activity ->
            IndeterminateProgressDialogFragment.newInstance(
                activity,
                R.string.processing_nsp,
                task
            ).show(activity.supportFragmentManager, IndeterminateProgressDialogFragment.TAG)
        } ?: run {
            task()
        }
    }

    // Storage methods for selected content
    fun saveSelectedUpdate(uri: Uri, version: String) {
        baseTitleId?.let { titleId ->
            prefs.edit()
                .putString(getUpdateKey(titleId), uri.toString())
                .putString(getUpdateVersionKey(titleId), version)
                .apply()
        }
    }

    private fun addSelectedDlc(uri: Uri, name: String) {
        baseTitleId?.let { titleId ->
            val currentDlcs = getSelectedDlcs().toMutableSet()
            val currentNames = getDlcNames().toMutableList()
            
            currentDlcs.add(uri.toString())
            currentNames.add(name)
            
            prefs.edit()
                .putStringSet(getDlcListKey(titleId), currentDlcs)
                .putString(getDlcNamesKey(titleId), currentNames.joinToString("|"))
                .apply()
        }
    }

    private fun getSelectedUpdate(): Uri? {
        return baseTitleId?.let { titleId ->
            prefs.getString(getUpdateKey(titleId), null)?.let { Uri.parse(it) }
        }
    }

    private fun getSelectedDlcs(): Set<String> {
        return baseTitleId?.let { titleId ->
            prefs.getStringSet(getDlcListKey(titleId), emptySet()) ?: emptySet()
        } ?: emptySet()
    }

    private fun getUpdateVersion(): String? {
        return baseTitleId?.let { titleId ->
            prefs.getString(getUpdateVersionKey(titleId), null)
        }
    }

    private fun getDlcNames(): List<String> {
        return baseTitleId?.let { titleId ->
            prefs.getString(getDlcNamesKey(titleId), "")?.split("|")?.filter { it.isNotEmpty() } ?: emptyList()
        } ?: emptyList()
    }

    // Public methods to get selected content for game launch
    fun getSelectedUpdateUri(): Uri? {
        return baseTitleId?.let { titleId ->
            org.stratoemu.strato.ContentManager.getEnabledUpdate(titleId)?.uri
        }
    }
    
    fun getSelectedDlcUris(): List<Uri> {
        return baseTitleId?.let { titleId ->
            org.stratoemu.strato.ContentManager.getEnabledDlcs(titleId).map { it.uri }
        } ?: emptyList()
    }

    fun clearSelectedContent() {
        baseTitleId?.let { titleId ->
            prefs.edit()
                .remove(getUpdateKey(titleId))
                .remove(getDlcListKey(titleId))
                .remove(getUpdateVersionKey(titleId))
                .remove(getDlcNamesKey(titleId))
                .apply()
            updateSummary()
        }
    }
    
    fun clearUpdate() {
        baseTitleId?.let { titleId ->
            prefs.edit()
                .remove(getUpdateKey(titleId))
                .remove(getUpdateVersionKey(titleId))
                .apply()
            updateSummary()
        }
    }
    
    fun addDlc(uri: Uri, name: String) {
        baseTitleId?.let { titleId ->
            val currentDlcs = getSelectedDlcs().toMutableSet()
            val currentNames = getDlcNames().toMutableList()
            
            // Avoid duplicates
            if (!currentDlcs.contains(uri.toString())) {
                currentDlcs.add(uri.toString())
                currentNames.add(name)
                
                prefs.edit()
                    .putStringSet(getDlcListKey(titleId), currentDlcs)
                    .putString(getDlcNamesKey(titleId), currentNames.joinToString("|"))
                    .apply()
                updateSummary()
            }
        }
    }
    
    fun removeDlc(uri: Uri) {
        baseTitleId?.let { titleId ->
            val currentDlcs = getSelectedDlcs().toMutableSet()
            val currentNames = getDlcNames().toMutableList()
            val uriString = uri.toString()
            
            // Find the index to remove both URI and name at the same position
            val dlcList = currentDlcs.toList()
            val indexToRemove = dlcList.indexOf(uriString)
            
            if (indexToRemove != -1 && indexToRemove < currentNames.size) {
                currentDlcs.remove(uriString)
                currentNames.removeAt(indexToRemove)
                
                prefs.edit()
                    .putStringSet(getDlcListKey(titleId), currentDlcs)
                    .putString(getDlcNamesKey(titleId), currentNames.joinToString("|"))
                    .apply()
                updateSummary()
            }
        }
    }

    private fun showErrorMessage(messageResId: Int) {
        val activity = (context as? SettingsActivity) ?: return
        Snackbar.make(activity.binding.root, messageResId, Snackbar.LENGTH_LONG).show()
    }
    
    private fun showSuccessMessage(message: String) {
        val activity = (context as? SettingsActivity) ?: return
        Snackbar.make(activity.binding.root, message, Snackbar.LENGTH_SHORT).show()
    }
    
    private fun extractBaseTitleId(metadata: org.stratoemu.strato.loader.NspMetadata): String? {
        return when {
            metadata.isUpdate -> {
                // For updates, use parentTitleId if available, otherwise use titleId
                metadata.parentTitleId?.takeIf { it.isNotEmpty() } ?: metadata.titleId
            }
            metadata.isDlc -> {
                // For DLC, use parentTitleId if available, otherwise derive from titleId
                metadata.parentTitleId?.takeIf { it.isNotEmpty() } ?: run {
                    val titleIdLong = metadata.titleId.toLongOrNull(16) ?: return null
                    val baseTitleIdLong = (titleIdLong and -0x1000L) // Clear last 3 hex digits
                    String.format("%016X", baseTitleIdLong)
                }
            }
            else -> null
        }
    }
    
    private fun openManageContentActivity() {
        val activity = (context as? SettingsActivity) ?: return
        
        // Get the AppItem from SettingsActivity intent
        val appItem = activity.intent.serializable<org.stratoemu.strato.data.BaseAppItem>(org.stratoemu.strato.data.AppItemTag)
        
        if (appItem != null) {
            val intent = android.content.Intent(activity, org.stratoemu.strato.ManageContentActivity::class.java)
            intent.putExtra(org.stratoemu.strato.data.AppItemTag, appItem as java.io.Serializable)
            activity.startActivity(intent)
        }
    }
}