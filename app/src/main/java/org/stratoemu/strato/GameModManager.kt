/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import java.io.File
import java.io.FileOutputStream
import java.util.Locale
import java.util.UUID
import java.util.zip.ZipInputStream

data class GameMod(
    val name: String,
    val directory: File,
    val enabled: Boolean
)

class GameModManager(
    private val context: Context,
    titleId: String
) {
    private val normalizedTitleId = titleId.uppercase(Locale.ROOT)
    private val root = File(
        context.getPublicFilesDir(),
        "switch/load/$normalizedTitleId"
    )

    fun listMods(): List<GameMod> {
        if (!root.isDirectory)
            return emptyList()

        return root.listFiles()
            ?.asSequence()
            ?.filter { it.isDirectory && !it.name.startsWith('.') && isModPackage(it) }
            ?.sortedBy { it.name.lowercase(Locale.ROOT) }
            ?.map { directory ->
                GameMod(
                    name = directory.name,
                    directory = directory,
                    enabled = !File(directory, DisabledMarker).exists()
                )
            }
            ?.toList()
            ?: emptyList()
    }

    fun setEnabled(mod: GameMod, enabled: Boolean) {
        val marker = File(mod.directory, DisabledMarker)
        if (enabled) {
            if (marker.exists() && !marker.delete())
                throw IllegalStateException("Failed to enable mod ${mod.name}")
        } else if (!marker.exists() && !marker.createNewFile()) {
            throw IllegalStateException("Failed to disable mod ${mod.name}")
        }
    }

    fun remove(mod: GameMod) {
        if (mod.directory.exists() && !mod.directory.deleteRecursively())
            throw IllegalStateException("Failed to delete mod ${mod.name}")
    }

    fun importArchive(uri: Uri, displayName: String): Int {
        val tempRoot = createTempRoot()
        try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                ZipInputStream(input.buffered()).use { zip ->
                    val rootPath = tempRoot.canonicalPath + File.separator
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        val entryName = entry.name.replace('\\', '/')
                        if (entryName.startsWith('/'))
                            throw IllegalArgumentException("Invalid archive path")

                        val target = File(tempRoot, entryName).canonicalFile
                        if (!target.path.startsWith(rootPath))
                            throw IllegalArgumentException("Invalid archive path")

                        if (entry.isDirectory) {
                            if (!target.isDirectory && !target.mkdirs())
                                throw IllegalStateException("Failed to create archive directory")
                        } else {
                            target.parentFile?.let { parent ->
                                if (!parent.isDirectory && !parent.mkdirs())
                                    throw IllegalStateException("Failed to create archive directory")
                            }
                            FileOutputStream(target).use { output -> zip.copyTo(output) }
                        }
                        zip.closeEntry()
                    }
                }
            } ?: throw IllegalArgumentException("Unable to open archive")

            val fallbackName = displayName.substringBeforeLast('.').ifBlank { "Mod" }
            return installDiscoveredPackages(tempRoot, fallbackName)
        } finally {
            tempRoot.deleteRecursively()
        }
    }

    fun importFolder(uri: Uri): Int {
        val documentRoot = DocumentFile.fromTreeUri(context, uri)
            ?: throw IllegalArgumentException("Unable to open folder")
        val tempRoot = createTempRoot()

        try {
            copyDocumentTree(documentRoot, tempRoot)
            return installDiscoveredPackages(tempRoot, documentRoot.name ?: "Mod")
        } finally {
            tempRoot.deleteRecursively()
        }
    }

    private fun createTempRoot(): File {
        val directory = File(context.cacheDir, "mod-import-${UUID.randomUUID()}")
        if (!directory.mkdirs())
            throw IllegalStateException("Failed to create import directory")
        return directory
    }

    private fun installDiscoveredPackages(sourceRoot: File, fallbackName: String): Int {
        val packages = discoverPackages(sourceRoot)
        if (packages.isEmpty())
            throw IllegalArgumentException("Package does not contain supported ExeFS, RomFS, ExeFS patches, or cheats for this game")

        if (!root.isDirectory && !root.mkdirs())
            throw IllegalStateException("Failed to create game mod directory")

        val usedNames = mutableSetOf<String>()
        packages.forEachIndexed { index, source ->
            val requestedName = if (
                source == sourceRoot || source.name.equals(normalizedTitleId, ignoreCase = true)
            ) fallbackName else source.name
            val baseName = safeName(requestedName, index)
            var packageName = baseName
            var suffix = 2
            while (!usedNames.add(packageName))
                packageName = "$baseName (${suffix++})"
            installPackage(source, packageName)
        }
        return packages.size
    }

    private fun discoverPackages(directory: File, depth: Int = 0): List<File> {
        if (isTitleIdDirectory(directory) && !directory.name.equals(normalizedTitleId, ignoreCase = true))
            return emptyList()
        if (isModPackage(directory))
            return listOf(directory)
        if (depth >= MaxDiscoveryDepth)
            return emptyList()

        return directory.listFiles()
            ?.asSequence()
            ?.filter { it.isDirectory && !it.name.startsWith('.') }
            ?.flatMap { discoverPackages(it, depth + 1).asSequence() }
            ?.toList()
            ?: emptyList()
    }

    private fun installPackage(source: File, packageName: String) {
        val staging = File(root, ".${packageName}.importing-${UUID.randomUUID()}")
        val target = File(root, packageName)
        try {
            copyDirectory(source, staging)
            if (!isModPackage(staging))
                throw IllegalArgumentException("Package does not contain supported ExeFS, RomFS, ExeFS patches, or cheats")

            if (target.exists() && !target.deleteRecursively())
                throw IllegalStateException("Failed to replace existing mod")

            if (!staging.renameTo(target)) {
                copyDirectory(staging, target)
                if (!staging.deleteRecursively())
                    throw IllegalStateException("Failed to finalize mod import")
            }
        } catch (e: Exception) {
            staging.deleteRecursively()
            throw e
        }
    }

    private fun copyDirectory(source: File, target: File) {
        if (source.isDirectory) {
            if (!target.isDirectory && !target.mkdirs())
                throw IllegalStateException("Failed to create mod directory")
            source.listFiles()?.forEach { child ->
                if (child.name != DisabledMarker)
                    copyDirectory(child, File(target, child.name))
            }
        } else {
            target.parentFile?.let { parent ->
                if (!parent.isDirectory && !parent.mkdirs())
                    throw IllegalStateException("Failed to create mod directory")
            }
            source.inputStream().buffered().use { input ->
                target.outputStream().buffered().use { output -> input.copyTo(output) }
            }
        }
    }

    private fun copyDocumentTree(source: DocumentFile, target: File) {
        if (source.isDirectory) {
            if (!target.isDirectory && !target.mkdirs())
                throw IllegalStateException("Failed to create import directory")
            source.listFiles().forEach { child ->
                val name = child.name ?: return@forEach
                if (name == DisabledMarker)
                    return@forEach
                copyDocumentTree(child, safeChild(target, name))
            }
        } else if (source.isFile) {
            target.parentFile?.let { parent ->
                if (!parent.isDirectory && !parent.mkdirs())
                    throw IllegalStateException("Failed to create import directory")
            }
            context.contentResolver.openInputStream(source.uri)?.use { input ->
                target.outputStream().buffered().use { output -> input.copyTo(output) }
            } ?: throw IllegalArgumentException("Unable to read ${source.name}")
        }
    }

    private fun safeChild(parent: File, name: String): File {
        val parentPath = parent.canonicalPath + File.separator
        val child = File(parent, name).canonicalFile
        if (!child.path.startsWith(parentPath))
            throw IllegalArgumentException("Invalid document path")
        return child
    }

    private fun isModPackage(directory: File): Boolean {
        return findChildDirectory(directory, "exefs") != null ||
            findChildDirectory(directory, "romfs") != null ||
            findChildDirectory(directory, "exefs_patches") != null ||
            findChildDirectory(directory, "cheats") != null
    }

    private fun isTitleIdDirectory(directory: File): Boolean {
        return TitleIdPattern.matches(directory.name)
    }

    private fun findChildDirectory(parent: File, name: String): File? {
        return parent.listFiles()?.firstOrNull {
            it.isDirectory && it.name.equals(name, ignoreCase = true)
        }
    }

    private fun safeName(rawName: String, index: Int): String {
        return rawName
            .trim()
            .replace(InvalidFilenameChars, "_")
            .trim('.', ' ')
            .take(MaxNameLength)
            .ifBlank { "Mod${if (index == 0) "" else " ${index + 1}"}" }
    }

    companion object {
        const val DisabledMarker = ".disabled"
        private const val MaxDiscoveryDepth = 4
        private const val MaxNameLength = 96
        private val InvalidFilenameChars = Regex("[\\\\/:*?\"<>|]")
        private val TitleIdPattern = Regex("^[0-9A-Fa-f]{16}$")
    }
}
