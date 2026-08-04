/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2025 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import android.util.Log
import androidx.preference.PreferenceManager
import androidx.documentfile.provider.DocumentFile

enum class ContentType {
    UPDATE, DLC
}

data class ContentItem(
    val uri: Uri,
    val name: String,
    val version: String? = null,
    val type: ContentType,
    val isEnabled: Boolean = false,
    val titleId: String,
    val displayName: String? = null
)

/**
 * Manages game content (updates and DLC) storage and state
 */
object ContentManager {
    
    private fun getPrefs(context: Context): SharedPreferences {
        return PreferenceManager.getDefaultSharedPreferences(context)
    }
    
    private fun getUpdateListKey(titleId: String) = "content_updates_$titleId"
    private fun getDlcListKey(titleId: String) = "content_dlcs_$titleId" 
    private fun getUpdateStatesKey(titleId: String) = "content_update_states_$titleId"
    private fun getDlcStatesKey(titleId: String) = "content_dlc_states_$titleId"
    
    fun addUpdate(titleId: String, uri: Uri, name: String, version: String) {
        val context = StratoApplication.instance
        val prefs = getPrefs(context)
        
        // Get existing updates
        val existing = getUpdatesForGame(titleId).toMutableList()
        
        // Remove any existing update with same URI (replace)
        existing.removeAll { it.uri == uri }
        
        // Add new update (disabled by default, only one update can be active)
        existing.add(ContentItem(uri, name, version, ContentType.UPDATE, false, titleId))
        
        // Save updates
        val updateData = existing.map { "${it.uri}|${it.name}|${it.version}" }
        prefs.edit()
            .putStringSet(getUpdateListKey(titleId), updateData.toSet())
            .apply()
    }
    
    fun addDlc(titleId: String, uri: Uri, name: String, displayName: String? = null) {
        val context = StratoApplication.instance
        val prefs = getPrefs(context)

        val existing = getDlcsForGame(titleId).toMutableList()

        existing.removeAll { it.uri == uri }

        val newDlc = ContentItem(uri, name, null, ContentType.DLC, false, titleId, displayName)
        existing.add(newDlc)

        val dlcData = existing.map {
            if (it.displayName != null) {
                "${it.uri}|${it.name}|${it.displayName}"
            } else {
                "${it.uri}|${it.name}"
            }
        }

        prefs.edit()
            .putStringSet(getDlcListKey(titleId), dlcData.toSet())
            .apply()
    }
    
    fun getUpdatesForGame(titleId: String): List<ContentItem> {
        val context = StratoApplication.instance
        val prefs = getPrefs(context)
        
        val updateData = prefs.getStringSet(getUpdateListKey(titleId), emptySet()) ?: emptySet()
        val enabledUpdates = prefs.getStringSet(getUpdateStatesKey(titleId), emptySet()) ?: emptySet()
        
        return updateData.mapNotNull { data ->
            val parts = data.split("|")
            if (parts.size >= 3) {
                val uri = Uri.parse(parts[0])
                val name = parts[1]
                val version = parts[2]
                val isEnabled = enabledUpdates.contains(parts[0])
                ContentItem(uri, name, version, ContentType.UPDATE, isEnabled, titleId)
            } else null
        }
    }
    
    fun getDlcsForGame(titleId: String): List<ContentItem> {
        val context = StratoApplication.instance
        val prefs = getPrefs(context)

        val dlcData = prefs.getStringSet(getDlcListKey(titleId), emptySet()) ?: emptySet()
        val enabledDlcs = prefs.getStringSet(getDlcStatesKey(titleId), emptySet()) ?: emptySet()

        val dlcs = dlcData.mapNotNull { data ->
            try {
                val parts = data.split("|")
                if (parts.size >= 2) {
                    val uri = Uri.parse(parts[0])
                    val name = parts[1]
                    val displayName = if (parts.size >= 3) parts[2] else null
                    val isEnabled = enabledDlcs.contains(parts[0])
                    ContentItem(uri, name, null, ContentType.DLC, isEnabled, titleId, displayName)
                } else {
                    null
                }
            } catch (e: Exception) {
                null
            }
        }

        return dlcs
    }
    
    fun getContentForGame(titleId: String): List<ContentItem> {
        val updates = getUpdatesForGame(titleId)
        val dlcs = getDlcsForGame(titleId)
        return (updates + dlcs).sortedBy { it.name }
    }
    
    fun setUpdateEnabled(titleId: String, uri: Uri, enabled: Boolean) {
        val context = StratoApplication.instance
        val prefs = getPrefs(context)
        
        val currentEnabled = prefs.getStringSet(getUpdateStatesKey(titleId), emptySet())?.toMutableSet() ?: mutableSetOf()
        
        if (enabled) {
            // Disable all other updates first (only one update can be active)
            currentEnabled.clear()
            currentEnabled.add(uri.toString())
        } else {
            currentEnabled.remove(uri.toString())
        }
        
        prefs.edit()
            .putStringSet(getUpdateStatesKey(titleId), currentEnabled)
            .apply()
    }
    
    fun setDlcEnabled(titleId: String, uri: Uri, enabled: Boolean) {
        val context = StratoApplication.instance
        val prefs = getPrefs(context)
        
        val currentEnabled = prefs.getStringSet(getDlcStatesKey(titleId), emptySet())?.toMutableSet() ?: mutableSetOf()
        
        if (enabled) {
            currentEnabled.add(uri.toString())
        } else {
            currentEnabled.remove(uri.toString())
        }
        
        prefs.edit()
            .putStringSet(getDlcStatesKey(titleId), currentEnabled)
            .apply()
    }
    
    fun removeContent(titleId: String, uri: Uri) {
        val context = StratoApplication.instance
        val prefs = getPrefs(context)
        
        // Remove from updates
        val updates = prefs.getStringSet(getUpdateListKey(titleId), emptySet())?.toMutableSet() ?: mutableSetOf()
        updates.removeAll { it.startsWith(uri.toString()) }
        
        // Remove from DLCs
        val dlcs = prefs.getStringSet(getDlcListKey(titleId), emptySet())?.toMutableSet() ?: mutableSetOf()
        dlcs.removeAll { it.startsWith(uri.toString()) }
        
        // Remove from enabled states
        val enabledUpdates = prefs.getStringSet(getUpdateStatesKey(titleId), emptySet())?.toMutableSet() ?: mutableSetOf()
        enabledUpdates.remove(uri.toString())
        
        val enabledDlcs = prefs.getStringSet(getDlcStatesKey(titleId), emptySet())?.toMutableSet() ?: mutableSetOf()
        enabledDlcs.remove(uri.toString())
        
        prefs.edit()
            .putStringSet(getUpdateListKey(titleId), updates)
            .putStringSet(getDlcListKey(titleId), dlcs)
            .putStringSet(getUpdateStatesKey(titleId), enabledUpdates)
            .putStringSet(getDlcStatesKey(titleId), enabledDlcs)
            .apply()
    }
    
    fun getEnabledUpdate(titleId: String): ContentItem? {
        return getUpdatesForGame(titleId).firstOrNull { it.isEnabled }
    }
    
    fun getEnabledDlcs(titleId: String): List<ContentItem> {
        return getDlcsForGame(titleId).filter { it.isEnabled }
    }
    
    /**
     * Cleans up content entries that no longer exist in storage
     * @return Pair of (removedUpdates, removedDlcs) counts
     */
    fun cleanupNonExistentContent(context: android.content.Context, titleId: String): Pair<Int, Int> {
        val prefs = getPrefs(context)
        var removedUpdates = 0
        var removedDlcs = 0
        
        // Clean up updates
        val updates = getUpdatesForGame(titleId)
        val validUpdates = updates.filter { update ->
            try {
                val file = DocumentFile.fromSingleUri(context, update.uri)
                file?.exists() == true
            } catch (e: Exception) {
                false
            }
        }
        
        if (validUpdates.size < updates.size) {
            removedUpdates = updates.size - validUpdates.size
            val updateData = validUpdates.map { "${it.uri}|${it.name}|${it.version}" }
            prefs.edit()
                .putStringSet(getUpdateListKey(titleId), updateData.toSet())
                .apply()
        }
        
        // Clean up DLCs
        val dlcs = getDlcsForGame(titleId)
        val validDlcs = dlcs.filter { dlc ->
            try {
                val file = DocumentFile.fromSingleUri(context, dlc.uri)
                file?.exists() == true
            } catch (e: Exception) {
                false
            }
        }
        
        if (validDlcs.size < dlcs.size) {
            removedDlcs = dlcs.size - validDlcs.size
            val dlcData = validDlcs.map {
                if (it.displayName != null) {
                    "${it.uri}|${it.name}|${it.displayName}"
                } else {
                    "${it.uri}|${it.name}"
                }
            }
            prefs.edit()
                .putStringSet(getDlcListKey(titleId), dlcData.toSet())
                .apply()
        }
        
        // Clean up enabled states for non-existent files
        val enabledUpdates = prefs.getStringSet(getUpdateStatesKey(titleId), emptySet())?.toMutableSet() ?: mutableSetOf()
        val enabledDlcs = prefs.getStringSet(getDlcStatesKey(titleId), emptySet())?.toMutableSet() ?: mutableSetOf()
        
        val allValidUris = (validUpdates + validDlcs).map { it.uri.toString() }.toSet()
        
        val initialEnabledCount = enabledUpdates.size + enabledDlcs.size
        
        enabledUpdates.retainAll(allValidUris)
        enabledDlcs.retainAll(allValidUris)
        
        if (enabledUpdates.size + enabledDlcs.size < initialEnabledCount) {
            prefs.edit()
                .putStringSet(getUpdateStatesKey(titleId), enabledUpdates)
                .putStringSet(getDlcStatesKey(titleId), enabledDlcs)
                .apply()
        }
        
        return Pair(removedUpdates, removedDlcs)
    }
}