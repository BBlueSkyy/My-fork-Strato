package org.stratoemu.strato

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import java.io.File
import java.io.IOException

/**
 * Imports one homebrew application and its companion files to the emulated SD card.
 *
 * The source is a SAF folder, not a single NRO, so relative assets, configuration
 * files and nested directories remain next to the executable after installation.
 * Launching the installed file from sdmc:/switch/ then lets libnx initialize its
 * working directory from the homebrew argv[0].
 */
internal class HomebrewInstaller(private val context: Context) {
    data class Result(val directoryName: String, val filesCopied: Int)

    private data class Entry(
        val document: DocumentFile,
        val relativePath: String,
        val isDirectory: Boolean
    )

    companion object {
        private const val MAX_DEPTH = 16
        private const val MAX_ENTRIES = 20000
    }

    fun install(sourceUri: Uri): Result {
        val source = DocumentFile.fromTreeUri(context, sourceUri)
            ?: throw IOException("Unable to access the selected folder")
        if (!source.isDirectory || !source.canRead())
            throw IOException("Select a readable folder containing one .nro file")

        val entries = ArrayList<Entry>()
        var nroCount = 0
        fun collect(directory: DocumentFile, prefix: String, depth: Int) {
            if (depth > MAX_DEPTH)
                throw IOException("Homebrew directory nesting exceeds $MAX_DEPTH levels")
            val children = directory.listFiles()
            for (child in children) {
                if (entries.size >= MAX_ENTRIES)
                    throw IOException("Homebrew folder contains too many files")
                val name = requireSafeName(child.name)
                val relative = if (prefix.isEmpty()) name else "$prefix/$name"
                when {
                    child.isDirectory -> {
                        entries.add(Entry(child, relative, true))
                        collect(child, relative, depth + 1)
                    }
                    child.isFile -> {
                        entries.add(Entry(child, relative, false))
                        if (name.endsWith(".nro", ignoreCase = true))
                            nroCount++
                    }
                    else -> throw IOException("Unsupported item: $relative")
                }
            }
        }
        collect(source, "", 0)

        if (nroCount != 1)
            throw IOException("Select a homebrew folder containing exactly one .nro file (found $nroCount)")

        val externalFiles = context.getExternalFilesDir(null)
            ?: throw IOException("The app's external files directory is unavailable")
        val sdSwitch = File(externalFiles, "switch/sdmc/switch")
        if (!sdSwitch.isDirectory && !sdSwitch.mkdirs())
            throw IOException("Unable to create the emulated SD homebrew directory")

        val sourceName = requireSafeName(source.name)
        val base = sourceName.substringBeforeLast('.', sourceName)
            .lowercase()
            .replace(Regex("[^a-z0-9_-]"), "-")
            .trim('-')
            .ifEmpty { "homebrew" }
            .take(64)

        // Never replace an existing homebrew installation, including one
        // whose name differs only by case (as on a Switch SD card).
        fun nameTaken(name: String) =
            sdSwitch.listFiles()?.any { it.name.equals(name, ignoreCase = true) } == true
        var destinationName = base
        var suffix = 2
        while (nameTaken(destinationName)) {
            destinationName = "$base-$suffix"
            suffix++
        }

        val destination = File(sdSwitch, destinationName)
        val staging = File(sdSwitch, ".$destinationName.install-${System.nanoTime()}")
        if (!staging.mkdir())
            throw IOException("Unable to prepare the homebrew installation")

        try {
            var copied = 0
            for (entry in entries) {
                val target = File(staging, entry.relativePath)
                if (entry.isDirectory) {
                    if (!target.isDirectory && !target.mkdirs())
                        throw IOException("Unable to create ${entry.relativePath}")
                } else {
                    val parent = target.parentFile
                    if (parent == null || (!parent.isDirectory && !parent.mkdirs()))
                        throw IOException("Unable to create ${entry.relativePath}")
                    val stream = context.contentResolver.openInputStream(entry.document.uri)
                        ?: throw IOException("Unable to read ${entry.relativePath}")
                    stream.use { input ->
                        target.outputStream().use { output -> input.copyTo(output) }
                    }
                    copied++
                }
            }

            // Staging and destination share a filesystem. Publishing only after
            // every file has been copied avoids half-installed entries in the UI.
            if (!staging.renameTo(destination))
                throw IOException("Unable to finish installing $destinationName")
            return Result(destinationName, copied)
        } catch (error: Exception) {
            staging.deleteRecursively()
            throw error
        }
    }

    private fun requireSafeName(name: String?): String {
        if (name.isNullOrEmpty() || name == "." || name == ".." ||
            name.any { it == '/' || it == '\\' || it == ':' || it.code < 32 })
            throw IOException("Invalid homebrew file or folder name")
        return name
    }
}
