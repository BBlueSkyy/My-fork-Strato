/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2025 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato.utils

import android.net.Uri
import android.view.View
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.Fragment

/**
 * Simple NSP file picker utility that uses Android's document picker
 */
class NspFilePicker private constructor(
    private val launcher: ActivityResultLauncher<Array<String>>,
    private val callback: (Uri, String) -> Unit
) {
    companion object {
        /**
         * Create NSP file picker for an Activity
         */
        fun with(activity: ComponentActivity, rootView: View, callback: (Uri, String) -> Unit): NspFilePicker {
            val launcher = activity.registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
                uri?.let { 
                    val fileName = getFileName(uri) ?: "Unknown"
                    callback(it, fileName)
                }
            }
            return NspFilePicker(launcher, callback)
        }

        /**
         * Create NSP file picker for a Fragment
         */
        fun with(fragment: Fragment, rootView: View, callback: (Uri, String) -> Unit): NspFilePicker {
            val launcher = fragment.registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
                uri?.let { 
                    val fileName = getFileName(uri) ?: "Unknown"
                    callback(it, fileName)
                }
            }
            return NspFilePicker(launcher, callback)
        }

        private fun getFileName(uri: Uri): String? {
            val path = uri.path ?: return null
            return path.substringAfterLast('/')
        }
    }

    /**
     * Open the file picker for NSP files
     */
    fun openNspFilePicker() {
        // Accept NSP files and generic application files
        launcher.launch(arrayOf("application/octet-stream", "*/*"))
    }

    /**
     * Cleanup resources (ActivityResultLauncher is automatically cleaned up)
     */
    fun cleanup() {
        // Nothing specific to clean up for ActivityResultLauncher
        // The launcher is automatically unregistered when activity/fragment is destroyed
    }
}