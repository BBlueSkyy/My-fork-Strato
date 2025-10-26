/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2025 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato.loader

import android.content.Context
import android.net.Uri

/**
 * Native parser for NSP files to extract metadata and validate content
 */
object NspParser {
    
    init {
        // The native library is loaded in StratoApplication
        // No need to load it again here
    }
    
    /**
     * Parse NSP metadata to determine content type and extract version/name
     * @param fd File descriptor of the NSP file
     * @param appFilesPath Path to internal app data storage
     * @return NspMetadata object or null if parsing failed
     */
    external fun parseNspMetadata(fd: Int, appFilesPath: String): NspMetadata?

    /**
     * Check if an NSP file is a valid update for a given base game
     * @param updateFd File descriptor of the update NSP
     * @param baseTitleId Title ID of the base game
     * @param appFilesPath Path to internal app data storage
     * @return true if valid update, false otherwise
     */
    external fun isValidUpdate(updateFd: Int, baseTitleId: String, appFilesPath: String): Boolean

    /**
     * Check if an NSP file is a valid DLC for a given base game
     * @param dlcFd File descriptor of the DLC NSP
     * @param baseTitleId Title ID of the base game
     * @param appFilesPath Path to internal app data storage
     * @return true if valid DLC, false otherwise
     */
    external fun isValidDlc(dlcFd: Int, baseTitleId: String, appFilesPath: String): Boolean

    /**
     * Get DLC name from NSP file
     * @param dlcFd File descriptor of the DLC NSP
     * @param appFilesPath Path to internal app data storage
     * @return DLC name or error message
     */
    external fun getDlcName(dlcFd: Int, appFilesPath: String): String

    /**
     * Get update version from NSP file
     * @param updateFd File descriptor of the update NSP
     * @param appFilesPath Path to internal app data storage
     * @return Update version or error message
     */
    external fun getUpdateVersion(updateFd: Int, appFilesPath: String): String

    /**
     * Convenience method to parse NSP metadata from URI
     */
    fun parseNspMetadata(context: Context, uri: Uri): NspMetadata? {
        android.util.Log.d("NspParser", "parseNspMetadata called with URI: $uri")
        return try {
            android.util.Log.d("NspParser", "Opening file descriptor for URI: $uri")
            val result = context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
                android.util.Log.d("NspParser", "File descriptor opened, calling native parseNspMetadata with fd: ${pfd.fd}")
                val appFilesPath = "${context.filesDir.canonicalPath}/"
                android.util.Log.d("NspParser", "App files path: $appFilesPath")
                val nativeResult = parseNspMetadata(pfd.fd, appFilesPath)
                android.util.Log.d("NspParser", "Native parseNspMetadata returned: $nativeResult")
                nativeResult
            }
            android.util.Log.d("NspParser", "Final result: $result")
            result
        } catch (e: Exception) {
            android.util.Log.e("NspParser", "Exception in parseNspMetadata", e)
            null
        }
    }

    /**
     * Convenience method to check if NSP is valid update from URI
     */
    fun isValidUpdate(context: Context, updateUri: Uri, baseTitleId: String): Boolean {
        return try {
            context.contentResolver.openFileDescriptor(updateUri, "r")?.use { pfd ->
                isValidUpdate(pfd.fd, baseTitleId, "${context.filesDir.canonicalPath}/")
            } ?: false
        } catch (e: Exception) {
            false
        }
    }

    /**
     * Convenience method to check if NSP is valid DLC from URI
     */
    fun isValidDlc(context: Context, dlcUri: Uri, baseTitleId: String): Boolean {
        return try {
            context.contentResolver.openFileDescriptor(dlcUri, "r")?.use { pfd ->
                isValidDlc(pfd.fd, baseTitleId, "${context.filesDir.canonicalPath}/")
            } ?: false
        } catch (e: Exception) {
            false
        }
    }

    /**
     * Get DLC name from URI
     */
    fun getDlcName(context: Context, dlcUri: Uri): String {
        return try {
            context.contentResolver.openFileDescriptor(dlcUri, "r")?.use { pfd ->
                getDlcName(pfd.fd, "${context.filesDir.canonicalPath}/")
            } ?: "Error reading DLC"
        } catch (e: Exception) {
            "Error reading DLC"
        }
    }

    /**
     * Get update version from URI
     */
    fun getUpdateVersion(context: Context, updateUri: Uri): String {
        return try {
            context.contentResolver.openFileDescriptor(updateUri, "r")?.use { pfd ->
                getUpdateVersion(pfd.fd, "${context.filesDir.canonicalPath}/")
            } ?: "Error reading version"
        } catch (e: Exception) {
            "Error reading version"
        }
    }
}