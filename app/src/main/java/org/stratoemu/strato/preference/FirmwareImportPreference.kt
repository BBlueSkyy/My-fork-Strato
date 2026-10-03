/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato.preference

import android.content.Context
import android.util.AttributeSet
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.preference.Preference
import androidx.preference.Preference.SummaryProvider
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.stratoemu.strato.FirmwareManager
import org.stratoemu.strato.R
import org.stratoemu.strato.fragments.IndeterminateProgressDialogFragment
import org.stratoemu.strato.settings.SettingsActivity

class FirmwareImportPreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = androidx.preference.R.attr.preferenceStyle
) : Preference(context, attrs, defStyleAttr) {
    private val documentPicker =
        (context as ComponentActivity).registerForActivityResult(
            ActivityResultContracts.OpenDocument()
        ) { uri ->
            uri?.let {
                val task: () -> Unit = {
                    val result = FirmwareManager.install(context, it)
                    val messageToShow = when (result.status) {
                        FirmwareManager.InstallStatus.Success ->
                            R.string.import_firmware_success

                        FirmwareManager.InstallStatus.InvalidContents ->
                            R.string.import_firmware_invalid_contents

                        FirmwareManager.InstallStatus.Error ->
                            R.string.import_firmware_failure
                    }

                    CoroutineScope(Dispatchers.Main).launch {
                        if (result.status == FirmwareManager.InstallStatus.Success)
                            notifyChanged()

                        Snackbar.make(
                            (context as SettingsActivity).binding.root,
                            messageToShow,
                            Snackbar.LENGTH_LONG
                        ).show()
                    }
                }

                IndeterminateProgressDialogFragment.newInstance(
                    context as SettingsActivity,
                    R.string.import_firmware_in_progress,
                    task
                ).show(
                    context.supportFragmentManager,
                    IndeterminateProgressDialogFragment.TAG
                )
            }
        }

    init {
        isEnabled = FirmwareManager.hasProductionKeys(context)

        summaryProvider = SummaryProvider<FirmwareImportPreference> { preference ->
            val defaultString =
                if (preference.isEnabled)
                    context.getString(R.string.firmware_not_installed)
                else
                    context.getString(R.string.firmware_keys_needed)

            getPersistedString(defaultString)
        }
    }

    override fun onClick() =
        documentPicker.launch(arrayOf("application/zip"))
}
