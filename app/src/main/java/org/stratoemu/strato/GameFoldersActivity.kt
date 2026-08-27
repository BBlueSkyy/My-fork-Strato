/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2025 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.content.Intent
import android.content.res.Configuration
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.MenuItem
import android.view.ViewGroup
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.content.res.AppCompatResources
import androidx.preference.PreferenceManager
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.stratoemu.strato.databinding.ActivityGameFoldersBinding
import org.stratoemu.strato.databinding.ItemGameFolderBinding
import org.stratoemu.strato.preference.FolderPickerPreference
import org.stratoemu.strato.settings.AppSettings

class GameFoldersActivity : AppCompatActivity() {
    private lateinit var binding: ActivityGameFoldersBinding
    private lateinit var foldersAdapter: GameFoldersAdapter

    private var folderBeingEdited: String? = null

    private val folderPicker =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            val oldFolder = folderBeingEdited
            folderBeingEdited = null

            if (uri == null)
                return@registerForActivityResult

            takeReadPermission(uri)

            if (oldFolder == null) {
                addFolder(uri)
            } else {
                replaceFolder(oldFolder, uri)
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityGameFoldersBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setupToolbar()
        setupFolderList()

        binding.fabAddFolder.setOnClickListener {
            folderBeingEdited = null
            folderPicker.launch(null)
        }

        loadFolders()
    }

    private fun setupToolbar() {
        setSupportActionBar(binding.toolbar)

        supportActionBar?.apply {
            title = getString(R.string.game_folders)
            setDisplayHomeAsUpEnabled(true)
            setDisplayShowHomeEnabled(true)

            val isDarkTheme =
                resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK ==
                    Configuration.UI_MODE_NIGHT_YES

            val textColor = if (isDarkTheme) Color.WHITE else Color.BLACK
            binding.toolbar.setTitleTextColor(textColor)

            val backArrow = AppCompatResources.getDrawable(
                this@GameFoldersActivity,
                androidx.appcompat.R.drawable.abc_ic_ab_back_material
            )
            backArrow?.setTint(textColor)
            setHomeAsUpIndicator(backArrow)
        }
    }

    private fun setupFolderList() {
        foldersAdapter = GameFoldersAdapter(
            onEdit = { folder ->
                folderBeingEdited = folder
                folderPicker.launch(Uri.parse(folder))
            },
            onDelete = { folder ->
                confirmDelete(folder)
            }
        )

        binding.recyclerViewFolders.apply {
            layoutManager = LinearLayoutManager(this@GameFoldersActivity)
            adapter = foldersAdapter
        }
    }

    private fun getSearchLocations(): MutableSet<String> {
        val prefs = PreferenceManager.getDefaultSharedPreferences(this)

        val locations = prefs
            .getStringSet(
                FolderPickerPreference.SEARCH_LOCATIONS_KEY,
                emptySet()
            )
            ?.filter { it.isNotBlank() }
            ?.toMutableSet()
            ?: mutableSetOf()

        if (locations.isEmpty()) {
            val legacyLocation = prefs.getString(LEGACY_SEARCH_LOCATION_KEY, "")
            if (!legacyLocation.isNullOrBlank()) {
                locations.add(legacyLocation)

                prefs.edit()
                    .putStringSet(
                        FolderPickerPreference.SEARCH_LOCATIONS_KEY,
                        HashSet(locations)
                    )
                    .apply()
            }
        }

        return locations
    }

    private fun persistLocations(
        locations: Set<String>,
        preferredLegacyLocation: String? = null
    ) {
        val prefs = PreferenceManager.getDefaultSharedPreferences(this)
        val currentLegacy = prefs.getString(LEGACY_SEARCH_LOCATION_KEY, "")

        val legacyLocation = when {
            preferredLegacyLocation != null &&
                locations.contains(preferredLegacyLocation) -> {
                preferredLegacyLocation
            }

            !currentLegacy.isNullOrBlank() &&
                locations.contains(currentLegacy) -> {
                currentLegacy
            }

            else -> {
                locations.sorted().firstOrNull().orEmpty()
            }
        }

        prefs.edit()
            .putStringSet(
                FolderPickerPreference.SEARCH_LOCATIONS_KEY,
                HashSet(locations)
            )
            .putString(LEGACY_SEARCH_LOCATION_KEY, legacyLocation)
            .apply()

        AppSettings(this).refreshRequired = true
        loadFolders()
    }

    private fun addFolder(uri: Uri) {
        val locations = getSearchLocations()
        val uriString = uri.toString()

        if (!locations.add(uriString)) {
            loadFolders()
            return
        }

        val prefs = PreferenceManager.getDefaultSharedPreferences(this)
        val currentLegacy = prefs.getString(LEGACY_SEARCH_LOCATION_KEY, "")

        persistLocations(
            locations,
            preferredLegacyLocation =
                if (currentLegacy.isNullOrBlank()) uriString else null
        )
    }

    private fun replaceFolder(oldFolder: String, newUri: Uri) {
        val locations = getSearchLocations()
        val newFolder = newUri.toString()

        if (!locations.remove(oldFolder)) {
            locations.add(newFolder)
            persistLocations(locations)
            return
        }

        locations.add(newFolder)

        val currentLegacy = PreferenceManager
            .getDefaultSharedPreferences(this)
            .getString(LEGACY_SEARCH_LOCATION_KEY, "")

        persistLocations(
            locations,
            preferredLegacyLocation =
                if (currentLegacy == oldFolder) newFolder else null
        )

        if (oldFolder != newFolder) {
            releaseReadPermission(Uri.parse(oldFolder))
        }
    }

    private fun confirmDelete(folder: String) {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.remove_game_folder)
            .setMessage(
                getString(
                    R.string.remove_game_folder_message,
                    displayPath(folder)
                )
            )
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.remove) { _, _ ->
                removeFolder(folder)
            }
            .show()
    }

    private fun removeFolder(folder: String) {
        val locations = getSearchLocations()

        if (!locations.remove(folder))
            return

        persistLocations(locations)
        releaseReadPermission(Uri.parse(folder))
    }

    private fun loadFolders() {
        val folders = getSearchLocations()
            .sortedBy { displayPath(it).lowercase() }

        foldersAdapter.submitFolders(folders)

        val isEmpty = folders.isEmpty()
        binding.emptyState.visibility =
            if (isEmpty) android.view.View.VISIBLE else android.view.View.GONE

        binding.recyclerViewFolders.visibility =
            if (isEmpty) android.view.View.GONE else android.view.View.VISIBLE
    }

    private fun takeReadPermission(uri: Uri) {
        try {
            contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        } catch (_: SecurityException) {
        }
    }

    private fun releaseReadPermission(uri: Uri) {
        try {
            contentResolver.releasePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        } catch (_: SecurityException) {
        }
    }

    private fun displayPath(uriString: String): String {
        val uri = Uri.parse(uriString)
        return Uri.decode(uri.path ?: uriString)
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        return when (item.itemId) {
            android.R.id.home -> {
                onBackPressedDispatcher.onBackPressed()
                true
            }

            else -> super.onOptionsItemSelected(item)
        }
    }

    companion object {
        private const val LEGACY_SEARCH_LOCATION_KEY = "search_location"
    }

    private inner class GameFoldersAdapter(
        private val onEdit: (String) -> Unit,
        private val onDelete: (String) -> Unit
    ) : RecyclerView.Adapter<GameFoldersAdapter.FolderViewHolder>() {

        private var folders: List<String> = emptyList()

        fun submitFolders(newFolders: List<String>) {
            folders = newFolders
            notifyDataSetChanged()
        }

        override fun onCreateViewHolder(
            parent: ViewGroup,
            viewType: Int
        ): FolderViewHolder {
            val itemBinding = ItemGameFolderBinding.inflate(
                LayoutInflater.from(parent.context),
                parent,
                false
            )
            return FolderViewHolder(itemBinding)
        }

        override fun onBindViewHolder(
            holder: FolderViewHolder,
            position: Int
        ) {
            holder.bind(folders[position])
        }

        override fun getItemCount(): Int = folders.size

        inner class FolderViewHolder(
            private val itemBinding: ItemGameFolderBinding
        ) : RecyclerView.ViewHolder(itemBinding.root) {

            fun bind(folder: String) {
                itemBinding.folderPath.text = displayPath(folder)
                itemBinding.folderType.text = getString(R.string.games)

                itemBinding.buttonEditFolder.setOnClickListener {
                    onEdit(folder)
                }

                itemBinding.buttonDeleteFolder.setOnClickListener {
                    onDelete(folder)
                }
            }
        }
    }
}
