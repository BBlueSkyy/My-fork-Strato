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
    private val launcher: ActivityResultLauncher<Array<String>>
) {
    companion object {
        /**
         * Create a single-selection NSP file picker for an Activity
         */
        fun with(
            activity: ComponentActivity,
            rootView: View,
            callback: (Uri, String) -> Unit
        ): NspFilePicker {
            val launcher = activity.registerForActivityResult(
                ActivityResultContracts.OpenDocument()
            ) { uri ->
                uri?.let {
                    val fileName = getFileName(it) ?: "Unknown"
                    callback(it, fileName)
                }
            }

            return NspFilePicker(launcher)
        }

        /**
         * Create a single-selection NSP file picker for a Fragment
         */
        fun with(
            fragment: Fragment,
            rootView: View,
            callback: (Uri, String) -> Unit
        ): NspFilePicker {
            val launcher = fragment.registerForActivityResult(
                ActivityResultContracts.OpenDocument()
            ) { uri ->
                uri?.let {
                    val fileName = getFileName(it) ?: "Unknown"
                    callback(it, fileName)
                }
            }

            return NspFilePicker(launcher)
        }

        /**
         * Create a multiple-selection NSP file picker for an Activity.
         *
         * There is no artificial file-count limit here. Every URI returned
         * by Android's document picker is delivered to the callback.
         */
        fun withMultiple(
            activity: ComponentActivity,
            rootView: View,
            callback: (Uri, String) -> Unit
        ): NspFilePicker {
            val launcher = activity.registerForActivityResult(
                ActivityResultContracts.OpenMultipleDocuments()
            ) { uris ->
                uris.forEach { uri ->
                    val fileName = getFileName(uri) ?: "Unknown"
                    callback(uri, fileName)
                }
            }

            return NspFilePicker(launcher)
        }

        /**
         * Create a multiple-selection NSP file picker for a Fragment.
         *
         * There is no artificial file-count limit here. Every URI returned
         * by Android's document picker is delivered to the callback.
         */
        fun withMultiple(
            fragment: Fragment,
            rootView: View,
            callback: (Uri, String) -> Unit
        ): NspFilePicker {
            val launcher = fragment.registerForActivityResult(
                ActivityResultContracts.OpenMultipleDocuments()
            ) { uris ->
                uris.forEach { uri ->
                    val fileName = getFileName(uri) ?: "Unknown"
                    callback(uri, fileName)
                }
            }

            return NspFilePicker(launcher)
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
