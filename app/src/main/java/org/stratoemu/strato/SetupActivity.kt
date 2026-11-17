/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.preference.PreferenceManager
import com.google.android.material.snackbar.Snackbar
import org.stratoemu.strato.databinding.ActivitySetupBinding
import org.stratoemu.strato.fragments.IndeterminateProgressDialogFragment
import org.stratoemu.strato.preference.FolderPickerPreference
import org.stratoemu.strato.settings.AppSettings
import java.util.HashSet

class SetupActivity : AppCompatActivity() {
    private val binding by lazy {
        ActivitySetupBinding.inflate(layoutInflater)
    }

    private val appSettings by lazy {
        AppSettings(this)
    }

    private val preferences
        get() = PreferenceManager.getDefaultSharedPreferences(this)

    private val keyPicker =
        registerForActivityResult(
            ActivityResultContracts.OpenDocument()
        ) { uri ->
            if (uri == null)
                return@registerForActivityResult

            takeReadPermission(uri)

            val result =
                KeyReader.import(
                    this,
                    uri,
                    KeyReader.KeyType.Prod
                )

            if (result == KeyReader.ImportResult.Success)
                appSettings.refreshRequired = true

            Snackbar.make(
                binding.root,
                resolveKeyImportResult(result),
                Snackbar.LENGTH_LONG
            ).show()

            updateState()
        }

    private val firmwarePicker =
        registerForActivityResult(
            ActivityResultContracts.OpenDocument()
        ) { uri ->
            if (uri == null)
                return@registerForActivityResult

            val task: () -> Unit = {
                val result =
                    FirmwareManager.install(
                        this,
                        uri
                    )

                runOnUiThread {
                    val message =
                        when (result.status) {
                            FirmwareManager.InstallStatus.Success ->
                                R.string.import_firmware_success

                            FirmwareManager.InstallStatus.InvalidContents ->
                                R.string.import_firmware_invalid_contents

                            FirmwareManager.InstallStatus.Error ->
                                R.string.import_firmware_failure
                        }

                    Snackbar.make(
                        binding.root,
                        message,
                        Snackbar.LENGTH_LONG
                    ).show()

                    updateState()
                }
            }

            IndeterminateProgressDialogFragment.newInstance(
                this,
                R.string.import_firmware_in_progress,
                task
            ).show(
                supportFragmentManager,
                IndeterminateProgressDialogFragment.TAG
            )
        }

    private val folderPicker =
        registerForActivityResult(
            ActivityResultContracts.OpenDocumentTree()
        ) { uri ->
            if (uri == null)
                return@registerForActivityResult

            takeReadPermission(uri)
            addGameFolder(uri)

            Snackbar.make(
                binding.root,
                R.string.initial_setup_game_folder_added,
                Snackbar.LENGTH_SHORT
            ).show()

            updateState()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        PreferenceManager.setDefaultValues(
            this,
            R.xml.app_preferences,
            false
        )

        setContentView(binding.root)

        binding.buttonKeys.setOnClickListener {
            keyPicker.launch(arrayOf("*/*"))
        }

        binding.buttonFirmware.setOnClickListener {
            firmwarePicker.launch(
                arrayOf("application/zip")
            )
        }

        binding.buttonGames.setOnClickListener {
            folderPicker.launch(null)
        }

        binding.buttonDone.setOnClickListener {
            finishSetup()
        }

        updateState()
    }

    private fun updateState() {
        val hasKeys =
            FirmwareManager.hasProductionKeys(this)

        binding.keysStatus.text =
            getString(
                if (hasKeys)
                    R.string.initial_setup_status_ready
                else
                    R.string.initial_setup_status_required
            )

        val firmwareVersion =
            FirmwareManager.installedVersion(this)

        binding.firmwareStatus.text =
            if (firmwareVersion == null) {
                getString(
                    if (hasKeys)
                        R.string.firmware_not_installed
                    else
                        R.string.firmware_keys_needed
                )
            } else {
                getString(
                    R.string.initial_setup_firmware_version,
                    firmwareVersion
                )
            }

        binding.buttonFirmware.isEnabled =
            hasKeys

        val folderCount =
            getGameFolders().size

        binding.gamesStatus.text =
            resources.getQuantityString(
                R.plurals.game_folder_count,
                folderCount,
                folderCount
            )

        binding.buttonDone.isEnabled =
            hasKeys && folderCount > 0
    }

    private fun getGameFolders(): MutableSet<String> {
        val locations =
            preferences
                .getStringSet(
                    FolderPickerPreference.SEARCH_LOCATIONS_KEY,
                    emptySet()
                )
                ?.filter { it.isNotBlank() }
                ?.toMutableSet()
                ?: mutableSetOf()

        if (
            locations.isEmpty() &&
            appSettings.searchLocation.isNotBlank()
        ) {
            locations.add(
                appSettings.searchLocation
            )
        }

        return locations
    }

    private fun addGameFolder(uri: Uri) {
        val locations =
            getGameFolders()

        val uriString =
            uri.toString()

        locations.add(uriString)

        preferences.edit()
            .putStringSet(
                FolderPickerPreference.SEARCH_LOCATIONS_KEY,
                HashSet(locations)
            )
            .apply()

        if (appSettings.searchLocation.isBlank())
            appSettings.searchLocation = uriString

        appSettings.refreshRequired = true
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

    private fun resolveKeyImportResult(
        result: KeyReader.ImportResult
    ): Int =
        when (result) {
            KeyReader.ImportResult.Success ->
                R.string.import_keys_success

            KeyReader.ImportResult.InvalidInputPath ->
                R.string.import_keys_invalid_input_path

            KeyReader.ImportResult.InvalidKeys ->
                R.string.import_keys_invalid_keys

            KeyReader.ImportResult.DeletePreviousFailed ->
                R.string.import_keys_delete_previous_failed

            KeyReader.ImportResult.MoveFailed ->
                R.string.import_keys_move_failed
        }

    private fun finishSetup() {
        appSettings.initialSetupCompleted = true

        startActivity(
            Intent(
                this,
                MainActivity::class.java
            ).addFlags(
                Intent.FLAG_ACTIVITY_CLEAR_TOP
            )
        )

        finish()
    }
}
