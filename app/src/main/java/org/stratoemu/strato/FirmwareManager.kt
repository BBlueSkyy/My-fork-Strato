/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.content.Context
import android.net.Uri
import androidx.annotation.Keep
import androidx.preference.PreferenceManager
import org.stratoemu.strato.utils.ZipUtils
import java.io.File
import java.io.FilenameFilter
import java.io.IOException

@Keep
object FirmwareManager {
    enum class InstallStatus {
        Success,
        InvalidContents,
        Error
    }

    data class InstallResult(
        val status: InstallStatus,
        val version: String = ""
    )

    private data class Firmware(
        val valid: Boolean,
        val version: String
    )

    private const val FIRMWARE_PREFERENCE_KEY = "firmware"

    fun hasProductionKeys(context: Context): Boolean =
        File(
            context.filesDir,
            "keys/${KeyReader.KeyType.Prod.fileName}"
        ).isFile

    fun installedVersion(context: Context): String? =
        PreferenceManager
            .getDefaultSharedPreferences(context)
            .getString(FIRMWARE_PREFERENCE_KEY, null)
            ?.takeIf { it.isNotBlank() }

    fun install(
        context: Context,
        uri: Uri
    ): InstallResult {
        val inputZip =
            context.contentResolver.openInputStream(uri)
                ?: return InstallResult(InstallStatus.Error)

        val firmwarePath =
            File(
                context.getPublicFilesDir().canonicalPath +
                    "/switch/nand/system/Contents/registered/"
            )

        val keysPath =
            "${context.filesDir.canonicalPath}/keys/"

        val fontsPath =
            "${context.getPublicFilesDir().canonicalPath}/fonts/"

        val cacheFirmwareDir =
            File("${context.cacheDir.path}/registered/")

        return try {
            cacheFirmwareDir.deleteRecursively()

            inputZip.use {
                ZipUtils.unzip(it, cacheFirmwareDir)
            }

            val firmware =
                isFirmwareValid(
                    cacheFirmwareDir,
                    keysPath
                )

            if (!firmware.valid) {
                InstallResult(InstallStatus.InvalidContents)
            } else {
                firmwarePath.deleteRecursively()

                if (!cacheFirmwareDir.copyRecursively(
                        firmwarePath,
                        overwrite = true
                    )
                ) {
                    InstallResult(InstallStatus.Error)
                } else {
                    PreferenceManager
                        .getDefaultSharedPreferences(context)
                        .edit()
                        .putString(
                            FIRMWARE_PREFERENCE_KEY,
                            firmware.version
                        )
                        .apply()

                    extractFonts(
                        firmwarePath.path,
                        keysPath,
                        fontsPath
                    )

                    InstallResult(
                        InstallStatus.Success,
                        firmware.version
                    )
                }
            }
        } catch (_: IOException) {
            InstallResult(InstallStatus.Error)
        } finally {
            cacheFirmwareDir.deleteRecursively()
        }
    }

    private fun isFirmwareValid(
        cacheFirmwareDir: File,
        keysPath: String
    ): Firmware {
        val filterNca =
            FilenameFilter { _, fileName ->
                fileName.endsWith(".nca")
            }

        val unfilteredNumOfFiles =
            cacheFirmwareDir.list()?.size ?: -1

        val filteredNumOfFiles =
            cacheFirmwareDir.list(filterNca)?.size ?: -2

        return if (
            unfilteredNumOfFiles ==
            filteredNumOfFiles
        ) {
            val version =
                fetchFirmwareVersion(
                    cacheFirmwareDir.path,
                    keysPath
                )

            Firmware(
                version.isNotEmpty(),
                version
            )
        } else {
            Firmware(false, "")
        }
    }

    private external fun fetchFirmwareVersion(
        systemArchivesPath: String,
        keysPath: String
    ): String

    private external fun extractFonts(
        systemArchivesPath: String,
        keysPath: String,
        fontsPath: String
    )
}
