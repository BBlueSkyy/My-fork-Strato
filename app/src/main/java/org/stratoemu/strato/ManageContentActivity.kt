/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2025 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.view.MenuItem
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.stratoemu.strato.data.AppItem
import org.stratoemu.strato.data.AppItemTag
import org.stratoemu.strato.databinding.ActivityManageContentBinding
import org.stratoemu.strato.preference.GameContentPreference
import org.stratoemu.strato.utils.NspFilePicker
import org.stratoemu.strato.utils.serializable

class ManageContentActivity : AppCompatActivity() {

    private lateinit var binding: ActivityManageContentBinding
    private lateinit var gameItem: AppItem
    private lateinit var gameContentPreference: GameContentPreference
    private lateinit var nspFilePicker: NspFilePicker
    private lateinit var modManager: GameModManager
    private lateinit var cheatManager: GameCheatManager
    private lateinit var updatesAdapter: ContentListAdapter
    private lateinit var dlcsAdapter: ContentListAdapter
    private lateinit var modsAdapter: ModListAdapter
    private lateinit var cheatsAdapter: CheatListAdapter

    private val modArchivePicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            val displayName = DocumentFile.fromSingleUri(this, uri)?.name ?: "mod.zip"
            importModArchive(uri, displayName)
        }
    }

    private val modFolderPicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null)
            importModFolder(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityManageContentBinding.inflate(layoutInflater)
        setContentView(binding.root)

        gameItem = intent.serializable<AppItem>(AppItemTag)
            ?: throw IllegalStateException("ManageContentActivity requires an AppItem")

        setSupportActionBar(binding.toolbar)
        supportActionBar?.apply {
            setDisplayHomeAsUpEnabled(true)
            setDisplayShowHomeEnabled(true)
            title = gameItem.title

            val isDarkTheme = resources.configuration.uiMode and
                android.content.res.Configuration.UI_MODE_NIGHT_MASK ==
                android.content.res.Configuration.UI_MODE_NIGHT_YES
            val textColor = if (isDarkTheme) android.graphics.Color.WHITE else android.graphics.Color.BLACK
            binding.toolbar.setTitleTextColor(textColor)

            val backArrow = androidx.appcompat.content.res.AppCompatResources.getDrawable(
                this@ManageContentActivity,
                androidx.appcompat.R.drawable.abc_ic_ab_back_material
            )
            backArrow?.setTint(textColor)
            setHomeAsUpIndicator(backArrow)
        }

        val titleId = gameItem.titleId ?: run {
            finish()
            return
        }

        gameContentPreference = GameContentPreference(this)
        gameContentPreference.setBaseTitleId(titleId)
        modManager = GameModManager(this, titleId)
        cheatManager = GameCheatManager(this, titleId)

        nspFilePicker = NspFilePicker.withMultiple(this, binding.root) { uri, fileName ->
            handleNspSelection(uri, fileName)
        }

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

        modsAdapter = ModListAdapter(
            onToggled = { mod, isEnabled -> handleModToggle(mod, isEnabled) },
            onDelete = { mod -> handleModDelete(mod) }
        )

        cheatsAdapter = CheatListAdapter(
            onToggled = { cheat, isEnabled -> handleCheatToggle(cheat, isEnabled) },
            onDelete = { cheat -> handleCheatDelete(cheat) }
        )

        binding.recyclerViewUpdates.apply {
            layoutManager = LinearLayoutManager(this@ManageContentActivity)
            adapter = updatesAdapter
        }
        binding.recyclerViewDlcs.apply {
            layoutManager = LinearLayoutManager(this@ManageContentActivity)
            adapter = dlcsAdapter
        }
        binding.recyclerViewMods.apply {
            layoutManager = LinearLayoutManager(this@ManageContentActivity)
            adapter = modsAdapter
        }
        binding.recyclerViewCheats.apply {
            layoutManager = LinearLayoutManager(this@ManageContentActivity)
            adapter = cheatsAdapter
        }

        binding.fabAddContent.setOnClickListener { showContentTypeDialog() }
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

    private fun showContentTypeDialog() {
        val choices = arrayOf(
            getString(R.string.updates_and_dlc),
            getString(R.string.mods_and_cheats)
        )
        var selected = 0

        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.content_type_title)
            .setSingleChoiceItems(choices, selected) { _, which -> selected = which }
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                if (selected == 0)
                    nspFilePicker.openNspFilePicker()
                else
                    showModImportDialog()
            }
            .show()
    }

    private fun showModImportDialog() {
        val choices = arrayOf(
            getString(R.string.mod_import_zip),
            getString(R.string.mod_import_folder)
        )
        var selected = 0

        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.mod_import_source)
            .setSingleChoiceItems(choices, selected) { _, which -> selected = which }
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                if (selected == 0) {
                    modArchivePicker.launch(
                        arrayOf(
                            "application/zip",
                            "application/x-zip-compressed",
                            "application/octet-stream"
                        )
                    )
                } else {
                    modFolderPicker.launch(null)
                }
            }
            .show()
    }

    private fun importModArchive(uri: Uri, displayName: String) {
        lifecycleScope.launch {
            try {
                val count = withContext(Dispatchers.IO) { modManager.importArchive(uri, displayName) }
                loadExistingContent()
                Snackbar.make(binding.root, getString(R.string.mod_import_success, count), Snackbar.LENGTH_LONG).show()
            } catch (e: Exception) {
                Log.e("ManageContent", "Failed to import add-on archive", e)
                val reason = e.message?.takeIf { it.isNotBlank() } ?: e.javaClass.simpleName
                Snackbar.make(binding.root, "${getString(R.string.mod_import_failed)}: $reason", Snackbar.LENGTH_LONG).show()
            }
        }
    }

    private fun importModFolder(uri: Uri) {
        lifecycleScope.launch {
            try {
                val count = withContext(Dispatchers.IO) { modManager.importFolder(uri) }
                loadExistingContent()
                Snackbar.make(binding.root, getString(R.string.mod_import_success, count), Snackbar.LENGTH_LONG).show()
            } catch (e: Exception) {
                Log.e("ManageContent", "Failed to import add-on folder", e)
                val reason = e.message?.takeIf { it.isNotBlank() } ?: e.javaClass.simpleName
                Snackbar.make(binding.root, "${getString(R.string.mod_import_failed)}: $reason", Snackbar.LENGTH_LONG).show()
            }
        }
    }

    private fun handleNspSelection(uri: Uri, fileName: String) {
        try {
            contentResolver.takePersistableUriPermission(
                uri,
                android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        } catch (_: Exception) {
        }

        val metadata = org.stratoemu.strato.loader.NspParser.parseNspMetadata(this, uri)
        if (metadata != null) {
            when {
                metadata.isUpdate -> {
                    if (org.stratoemu.strato.loader.NspParser.isValidUpdate(this, uri, gameItem.titleId!!))
                        addUpdate(uri, metadata)
                }
                metadata.isDlc -> {
                    if (org.stratoemu.strato.loader.NspParser.isValidDlc(this, uri, gameItem.titleId!!))
                        addDlc(uri, metadata, fileName)
                }
            }
        }
    }

    private fun addUpdate(uri: Uri, metadata: org.stratoemu.strato.loader.NspMetadata) {
        gameContentPreference.saveSelectedUpdate(uri, metadata.version)
        ContentManager.addUpdate(gameItem.titleId!!, uri, metadata.name, metadata.version)
        ContentManager.setUpdateEnabled(gameItem.titleId!!, uri, true)
        loadExistingContent()
    }

    private fun addDlc(uri: Uri, metadata: org.stratoemu.strato.loader.NspMetadata, fileName: String) {
        val titleId = gameItem.titleId!!
        val displayName = fileName.removeSuffix(".nsp")

        ContentManager.addDlc(titleId, uri, metadata.name, displayName)
        ContentManager.setDlcEnabled(titleId, uri, true)
        gameContentPreference.addDlc(uri, metadata.name)
        loadExistingContent()
        Log.d("ManageContent", "DLC added and enabled: ${metadata.name}")
    }

    private fun loadExistingContent() {
        val titleId = gameItem.titleId!!
        val updates = ContentManager.getUpdatesForGame(titleId)
        val dlcs = ContentManager.getDlcsForGame(titleId)
        val mods = modManager.listMods()
        val cheats = cheatManager.listCheats()

        updatesAdapter.submitList(updates)
        dlcsAdapter.submitList(dlcs)
        modsAdapter.submitList(mods)
        cheatsAdapter.submitList(cheats)

        val hasContent = updates.isNotEmpty() || dlcs.isNotEmpty() || mods.isNotEmpty() || cheats.isNotEmpty()
        binding.emptyState.visibility = if (hasContent) android.view.View.GONE else android.view.View.VISIBLE
        binding.contentScrollView.visibility = if (hasContent) android.view.View.VISIBLE else android.view.View.GONE
        binding.emptyUpdates.visibility = if (updates.isEmpty()) android.view.View.VISIBLE else android.view.View.GONE
        binding.emptyDlcs.visibility = if (dlcs.isEmpty()) android.view.View.VISIBLE else android.view.View.GONE
        binding.emptyMods.visibility = if (mods.isEmpty()) android.view.View.VISIBLE else android.view.View.GONE
        binding.emptyCheats.visibility = if (cheats.isEmpty()) android.view.View.VISIBLE else android.view.View.GONE
    }

    private fun handleModToggle(mod: GameMod, isEnabled: Boolean) {
        try {
            modManager.setEnabled(mod, isEnabled)
        } catch (e: Exception) {
            Log.e("ManageContent", "Failed to toggle mod ${mod.name}", e)
            Snackbar.make(binding.root, R.string.mod_operation_failed, Snackbar.LENGTH_LONG).show()
        }
        loadExistingContent()
    }

    private fun handleModDelete(mod: GameMod) {
        try {
            modManager.remove(mod)
        } catch (e: Exception) {
            Log.e("ManageContent", "Failed to delete mod ${mod.name}", e)
            Snackbar.make(binding.root, R.string.mod_operation_failed, Snackbar.LENGTH_LONG).show()
        }
        loadExistingContent()
    }

    private fun handleCheatToggle(cheat: GameCheat, isEnabled: Boolean) {
        try {
            cheatManager.setEnabled(cheat, isEnabled)
        } catch (e: Exception) {
            Log.e("ManageContent", "Failed to toggle cheat ${cheat.name}", e)
            Snackbar.make(binding.root, R.string.cheat_operation_failed, Snackbar.LENGTH_LONG).show()
        }
        loadExistingContent()
    }

    private fun handleCheatDelete(cheat: GameCheat) {
        try {
            cheatManager.remove(cheat)
        } catch (e: Exception) {
            Log.e("ManageContent", "Failed to delete cheat ${cheat.name}", e)
            Snackbar.make(binding.root, R.string.cheat_operation_failed, Snackbar.LENGTH_LONG).show()
        }
        loadExistingContent()
    }

    private fun handleUpdateToggle(contentItem: ContentItem, isEnabled: Boolean) {
        ContentManager.setUpdateEnabled(gameItem.titleId!!, contentItem.uri, isEnabled)

        if (isEnabled) {
            gameContentPreference.saveSelectedUpdate(contentItem.uri, contentItem.version ?: "")
            Log.d("ManageContent", "Update enabled in both systems: ${contentItem.name}")
        } else {
            gameContentPreference.clearUpdate()
            Log.d("ManageContent", "Update disabled in both systems: ${contentItem.name}")
        }
        loadExistingContent()
    }

    private fun handleDlcToggle(contentItem: ContentItem, isEnabled: Boolean) {
        ContentManager.setDlcEnabled(gameItem.titleId!!, contentItem.uri, isEnabled)
        if (isEnabled)
            gameContentPreference.addDlc(contentItem.uri, contentItem.name)
        else
            gameContentPreference.removeDlc(contentItem.uri)

        Log.d("ManageContent", "DLC ${if (isEnabled) "enabled" else "disabled"}: ${contentItem.name}")
    }

    private fun handleItemDelete(contentItem: ContentItem) {
        ContentManager.removeContent(gameItem.titleId!!, contentItem.uri)
        if (contentItem.type == ContentType.UPDATE)
            gameContentPreference.clearUpdate()
        else
            gameContentPreference.removeDlc(contentItem.uri)

        loadExistingContent()
        Log.d("ManageContent", "Content item deleted: ${contentItem.name}")
    }

    override fun onDestroy() {
        super.onDestroy()
        if (::nspFilePicker.isInitialized)
            nspFilePicker.cleanup()
    }
}
