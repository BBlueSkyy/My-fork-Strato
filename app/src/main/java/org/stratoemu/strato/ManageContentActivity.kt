/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2025 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.os.Bundle
import android.util.Log
import android.view.MenuItem
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import org.stratoemu.strato.data.AppItem
import org.stratoemu.strato.data.AppItemTag
import org.stratoemu.strato.databinding.ActivityManageContentBinding
import org.stratoemu.strato.preference.GameContentPreference
import org.stratoemu.strato.utils.NspFilePicker
import org.stratoemu.strato.utils.serializable

/**
 * Activity for managing game content (updates and DLC)
 */
class ManageContentActivity : AppCompatActivity() {
    
    private lateinit var binding: ActivityManageContentBinding
    private lateinit var gameItem: AppItem
    private lateinit var gameContentPreference: GameContentPreference
    private lateinit var nspFilePicker: NspFilePicker
    private lateinit var updatesAdapter: ContentListAdapter
    private lateinit var dlcsAdapter: ContentListAdapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityManageContentBinding.inflate(layoutInflater)
        setContentView(binding.root)
        
        // Get game item from intent
        gameItem = intent.serializable<AppItem>(AppItemTag)
            ?: throw IllegalStateException("ManageContentActivity requires an AppItem")
        
        // Setup toolbar
        setSupportActionBar(binding.toolbar)
        supportActionBar?.apply {
            setDisplayHomeAsUpEnabled(true)
            setDisplayShowHomeEnabled(true)
            
            title = gameItem.title
            
            val isDarkTheme = resources.configuration.uiMode and
                android.content.res.Configuration.UI_MODE_NIGHT_MASK == 
                android.content.res.Configuration.UI_MODE_NIGHT_YES
            
            val textColor = if (isDarkTheme) {
                android.graphics.Color.WHITE
            } else {
                android.graphics.Color.BLACK
            }
            
            binding.toolbar.setTitleTextColor(textColor)
            
            val backArrow = androidx.appcompat.content.res.AppCompatResources.getDrawable(
                this@ManageContentActivity,
                androidx.appcompat.R.drawable.abc_ic_ab_back_material
            )
            backArrow?.setTint(textColor)
            setHomeAsUpIndicator(backArrow)
        }
        
        // Setup game content preference
        gameItem.titleId?.let { titleId ->
            gameContentPreference = GameContentPreference(this)
            gameContentPreference.setBaseTitleId(titleId)
        } ?: run {
            finish()
            return
        }
        
        // Setup NSP file picker
        nspFilePicker = NspFilePicker.with(this, binding.root) { uri, fileName ->
            handleNspSelection(uri, fileName)
        }
        
        // Setup RecyclerViews
        updatesAdapter = ContentListAdapter(
            onUpdateToggled = { contentItem, isEnabled -> handleUpdateToggle(contentItem, isEnabled) },
            onDlcToggled = { _, _ -> },
            onItemDelete = { contentItem -> handleItemDelete(contentItem) }
        )
        dlcsAdapter = ContentListAdapter(
            onUpdateToggled = { _, _ -> },
            onDlcToggled = { contentItem, isEnabled -> handleDlcToggle(contentItem, isEnabled) },
            onItemDelete = { contentItem -> handleItemDelete(contentItem) }
        )

        binding.recyclerViewUpdates.apply {
            layoutManager = LinearLayoutManager(this@ManageContentActivity)
            adapter = updatesAdapter
        }
        binding.recyclerViewDlcs.apply {
            layoutManager = LinearLayoutManager(this@ManageContentActivity)
            adapter = dlcsAdapter
        }
        
        // Setup FAB
        binding.fabAddContent.setOnClickListener {
            nspFilePicker.openNspFilePicker()
        }
        
        // Load existing content
        loadExistingContent()
    }
    
    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        return when (item.itemId) {
            android.R.id.home -> {
                onBackPressed()
                true
            }
            else -> super.onOptionsItemSelected(item)
        }
    }
    
    private fun handleNspSelection(uri: android.net.Uri, fileName: String) {
        // Take persistent URI permissions first
        try {
            contentResolver.takePersistableUriPermission(uri, 
                android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (e: Exception) {
            // Continue anyway, might still work
        }
        
        // Parse NSP metadata
        val metadata = org.stratoemu.strato.loader.NspParser.parseNspMetadata(this, uri)
        
        if (metadata != null) {
            when {
                metadata.isUpdate -> {
                    // Validate it's an update for this game
                    if (org.stratoemu.strato.loader.NspParser.isValidUpdate(this, uri, gameItem.titleId!!)) {
                        addUpdate(uri, metadata)
                    }
                }
                metadata.isDlc -> {
                    if (org.stratoemu.strato.loader.NspParser.isValidDlc(this, uri, gameItem.titleId!!)) {
                        addDlc(uri, metadata, fileName)
                    }
                }
                else -> {}
            }
        }
    }
    
    private fun addUpdate(uri: android.net.Uri, metadata: org.stratoemu.strato.loader.NspMetadata) {
        // Save the update (this will replace any existing update)
        gameContentPreference.saveSelectedUpdate(uri, metadata.version)
        
        // Add to content manager and automatically enable it
        ContentManager.addUpdate(gameItem.titleId!!, uri, metadata.name, metadata.version)
        ContentManager.setUpdateEnabled(gameItem.titleId!!, uri, true)

        // Refresh the list
        loadExistingContent()
        
        
    }
    
    private fun addDlc(uri: android.net.Uri, metadata: org.stratoemu.strato.loader.NspMetadata, fileName: String) {
        val displayName = fileName.removeSuffix(".nsp")
        ContentManager.addDlc(gameItem.titleId!!, uri, metadata.name, displayName)

        loadExistingContent()


    }
    
    private fun loadExistingContent() {
        val titleId = gameItem.titleId!!
        val updates = ContentManager.getUpdatesForGame(titleId)
        val dlcs = ContentManager.getDlcsForGame(titleId)

        updatesAdapter.submitList(updates)
        dlcsAdapter.submitList(dlcs)

        val hasContent = updates.isNotEmpty() || dlcs.isNotEmpty()
        binding.emptyState.visibility =
            if (hasContent) android.view.View.GONE else android.view.View.VISIBLE
        binding.contentScrollView.visibility =
            if (hasContent) android.view.View.VISIBLE else android.view.View.GONE

        binding.emptyUpdates.visibility =
            if (updates.isEmpty()) android.view.View.VISIBLE else android.view.View.GONE
        binding.emptyDlcs.visibility =
            if (dlcs.isEmpty()) android.view.View.VISIBLE else android.view.View.GONE
    }
    
    private fun handleUpdateToggle(contentItem: ContentItem, isEnabled: Boolean) {
        // Update the content manager state FIRST
        ContentManager.setUpdateEnabled(gameItem.titleId!!, contentItem.uri, isEnabled)
        
        if (isEnabled) {
            // Enable this update (disable others first)
            gameContentPreference.saveSelectedUpdate(contentItem.uri, contentItem.version ?: "")
            Log.d("ManageContent", "Update enabled in both systems: ${contentItem.name}")
            
            // Verify it was saved correctly
            val savedUri = gameContentPreference.getSelectedUpdateUri()
            Log.d("ManageContent", "Verification - savedUri from GameContentPreference: $savedUri")
            val enabledFromContentManager = ContentManager.getEnabledUpdate(gameItem.titleId!!)
            Log.d("ManageContent", "Verification - enabled from ContentManager: ${enabledFromContentManager?.uri}")
        } else {
            // Disable update
            gameContentPreference.clearUpdate()
            Log.d("ManageContent", "Update disabled in both systems: ${contentItem.name}")
        }
        
        // Refresh list to update UI
        loadExistingContent()
    }
    
    private fun handleDlcToggle(contentItem: ContentItem, isEnabled: Boolean) {
        ContentManager.setDlcEnabled(gameItem.titleId!!, contentItem.uri, isEnabled)
        
        // Update preferences
        if (isEnabled) {
            gameContentPreference.addDlc(contentItem.uri, contentItem.name)
        } else {
            gameContentPreference.removeDlc(contentItem.uri)
        }
        
        Log.d("ManageContent", "DLC ${if (isEnabled) "enabled" else "disabled"}: ${contentItem.name}")
    }
    
    private fun handleItemDelete(contentItem: ContentItem) {
        // Remove from content manager
        ContentManager.removeContent(gameItem.titleId!!, contentItem.uri)
        
        // Remove from preferences if it was enabled
        if (contentItem.type == ContentType.UPDATE) {
            gameContentPreference.clearUpdate()
        } else {
            gameContentPreference.removeDlc(contentItem.uri)
        }
        
        // Refresh list
        loadExistingContent()

        Log.d("ManageContent", "Content item deleted: ${contentItem.name}")
    }
    
    override fun onDestroy() {
        super.onDestroy()
        if (::nspFilePicker.isInitialized) {
            nspFilePicker.cleanup()
        }
    }
}