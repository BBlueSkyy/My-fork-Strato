/*
 * SPDX-License-Identifier: MPL-2.0
 *
 * Diagnostic-only: export the latest built-in GPU capture through Strato's
 * existing DocumentsProvider, avoiding direct Android/data file browsing.
 */

package org.stratoemu.strato

import android.content.Context
import android.os.Build
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.RandomAccessFile
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

internal object GpuCaptureExport {
    private val captureFolderName = Regex("capture_[0-9]+")
    private const val MaxArchiveInputBytes = 32L * 1024 * 1024
    private const val MaxSingleFileBytes = 8L * 1024 * 1024
    private const val MaxFiles = 512
    private const val MaxLogTailBytes = 256L * 1024

    fun createArchive(context: Context): File {
        val base = context.getPublicFilesDir()
        val source = File(base, "gpu_capture/cosmic_shake")
        if (!source.isDirectory && !source.mkdirs())
            throw IOException("Could not create capture directory in app files")

        val latest = source.listFiles()
            ?.filter { it.isDirectory && captureFolderName.matches(it.name) }
            ?.maxByOrNull { it.name }

        val output = File(source, "capture-diagnostic.zip")
        val temp = File(source, "capture-diagnostic.partial")
        val selected = mutableListOf<File>()
        val safeRoot = latest?.canonicalPath?.plus(File.separator)
        var selectedBytes = 0L
        var skipped = 0

        if (latest != null && safeRoot != null) {
            for (file in latest.walkTopDown()) {
                if (!file.isFile || !file.canonicalPath.startsWith(safeRoot))
                    continue
                val fileSize = file.length()
                if (selected.size >= MaxFiles || fileSize < 0 ||
                    fileSize > MaxSingleFileBytes ||
                    fileSize > MaxArchiveInputBytes - selectedBytes) {
                    skipped++
                    continue
                }
                selectedBytes += fileSize
                selected.add(file)
            }
        }

        val emulatorLog = File(base, "logs/emulation.log")
        val status = buildString {
            appendLine("Strato bounded GPU capture diagnostic")
            appendLine("build=" + BuildConfig.VERSION_NAME)
            appendLine("package=" + BuildConfig.APPLICATION_ID)
            appendLine("android_api=" + Build.VERSION.SDK_INT)
            appendLine("capture_directory_exists=" + source.isDirectory)
            appendLine("arm_trigger_exists=" + File(source, "ARM_CAPTURE").exists())
            appendLine("capture_hint_exists=" + File(source,
                "ARM_CAPTURE.rename_this_file_when_ready").exists())
            appendLine("latest_capture=" + (latest?.name ?: "none"))
            appendLine("selected_files=" + selected.size)
            appendLine("selected_bytes=" + selectedBytes)
            appendLine("skipped_files=" + skipped)
            appendLine("log_exists=" + emulatorLog.isFile)
            appendLine("NOTE: no capture files means there may have been no completed trigger.")
            appendLine("GPU image readbacks need presented frames; crashes before those frames")
            appendLine("can only be investigated with the emulator log and other diagnostics.")
        }

        if (temp.exists() && !temp.delete())
            throw IOException("Cannot replace a previous temporary export")
        try {
            ZipOutputStream(FileOutputStream(temp).buffered()).use { zip ->
                zip.putNextEntry(ZipEntry("diagnostic-status.txt"))
                zip.write(status.toByteArray(Charsets.UTF_8))
                zip.closeEntry()

                if (emulatorLog.isFile) {
                    RandomAccessFile(emulatorLog, "r").use { log ->
                        val length = log.length()
                        val start = (length - MaxLogTailBytes).coerceAtLeast(0L)
                        log.seek(start)
                        zip.putNextEntry(ZipEntry("emulation-tail.txt"))
                        val buffer = ByteArray(8192)
                        var remaining = length - start
                        while (remaining > 0) {
                            val count = log.read(buffer, 0,
                                minOf(buffer.size.toLong(), remaining).toInt())
                            if (count <= 0) break
                            zip.write(buffer, 0, count)
                            remaining -= count
                        }
                        zip.closeEntry()
                    }
                }

                for (file in selected) {
                    val entryName = file.relativeTo(source).invariantSeparatorsPath
                    zip.putNextEntry(ZipEntry(entryName))
                    file.inputStream().buffered().use { it.copyTo(zip) }
                    zip.closeEntry()
                }
            }
            if (output.exists() && !output.delete())
                throw IOException("Cannot replace a previous capture export")
            if (!temp.renameTo(output))
                throw IOException("Cannot finalize the GPU capture export")
        } catch (error: Exception) {
            temp.delete()
            throw error
        }
        return output
    }
}
