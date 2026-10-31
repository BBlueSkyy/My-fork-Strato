/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.loader

import android.content.Context
import androidx.documentfile.provider.DocumentFile
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.util.Locale

/**
 * Library-only discovery of native Switch 2 decrypted ExeFS directories.
 *
 * Uses the existing SAF tree permission; no files are copied and no emulator
 * execution is attempted. Other extracted Switch games are left untouched.
 */
internal object Switch2ExtractedDiscovery {
    private const val META_MAGIC = 0x4154454d // "META" (little endian)
    private const val ACI0_MAGIC = 0x30494341 // "ACI0" (little endian)
    private const val BANANZA_PROGRAM_ID = 0x040074A01BF12000L

    fun inspect(context: Context, directory: DocumentFile, children: Array<DocumentFile>): AppEntry? {
        val exefs = children.firstOrNull {
            it.isDirectory && it.name.equals("exefs", ignoreCase = true)
        } ?: return null

        val components = try { exefs.listFiles() } catch (_: Exception) { return null }
        if (!components.any { it.isFile && it.name == "main" } ||
            !components.any { it.isFile && it.name == "rtld" }) return null

        val npdm = components.firstOrNull { it.isFile && it.name == "main.npdm" } ?: return null
        val programId = readProgramId(context, npdm) ?: return null
        if ((programId ushr 56) != 0x04L) return null

        val directoryName = directory.name?.takeIf { it.isNotBlank() } ?: "Switch 2 game"
        val displayName = if (programId == BANANZA_PROGRAM_ID) "Donkey Kong Bananza" else directoryName
        return AppEntry(
            name = displayName,
            version = "Nintendo Switch 2 - not playable",
            titleId = String.format(Locale.ROOT, "%016X", programId),
            addOnContentBaseId = "",
            author = "Nintendo Switch 2",
            icon = null,
            romType = RomType.Base,
            parentTitleId = "",
            format = RomFormat.SWITCH2_EXTRACTED,
            uri = directory.uri,
            loaderResult = LoaderResult.Success
        )
    }

    /** Parse only the NPDM META/ACI0 headers. Do not confuse directory names with NX2 detection. */
    private fun readProgramId(context: Context, document: DocumentFile): Long? = try {
        val descriptor = context.contentResolver.openFileDescriptor(document.uri, "r") ?: return null
        descriptor.use { pfd ->
            FileInputStream(pfd.fileDescriptor).use { input ->
                val channel = input.channel
                val meta = ByteBuffer.allocate(0x80).order(ByteOrder.LITTLE_ENDIAN)
                if (!readAt(channel, meta, 0L) || meta.getInt(0) != META_MAGIC) return null
                val aciOffset = meta.getInt(0x70).toLong() and 0xffffffffL
                val aciSize = meta.getInt(0x74).toLong() and 0xffffffffL
                if (aciOffset < 0x80L || aciOffset > 32L * 1024L * 1024L ||
                    aciSize < 0x18L || aciSize > 32L * 1024L * 1024L ||
                    aciOffset + aciSize > channel.size()) return null

                val aci = ByteBuffer.allocate(0x18).order(ByteOrder.LITTLE_ENDIAN)
                if (!readAt(channel, aci, aciOffset) || aci.getInt(0) != ACI0_MAGIC) return null
                aci.getLong(0x10)
            }
        }
    } catch (_: Exception) {
        null
    }

    private fun readAt(channel: FileChannel, buffer: ByteBuffer, start: Long): Boolean {
        var cursor = start
        while (buffer.hasRemaining()) {
            val count = channel.read(buffer, cursor)
            if (count <= 0) return false
            cursor += count
        }
        return true
    }
}
