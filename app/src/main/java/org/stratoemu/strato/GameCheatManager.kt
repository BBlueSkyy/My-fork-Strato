/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.content.Context
import java.io.File
import java.util.Locale

data class GameCheat(
    val name: String,
    val packName: String,
    val buildId: String,
    val file: File,
    val enabled: Boolean,
    val packEnabled: Boolean
)

class GameCheatManager(context: Context, titleId: String) {
    private val root = File(
        context.getPublicFilesDir(),
        "switch/load/${titleId.uppercase(Locale.ROOT)}"
    )

    fun listCheats(): List<GameCheat> {
        if (!root.isDirectory)
            return emptyList()

        val result = mutableListOf<GameCheat>()
        root.listFiles()
            ?.filter { it.isDirectory && !it.name.startsWith('.') }
            ?.sortedBy { it.name.lowercase(Locale.ROOT) }
            ?.forEach { pack ->
                val cheatsDir = findChildDirectory(pack, "cheats") ?: return@forEach
                val packEnabled = !File(pack, GameModManager.DisabledMarker).exists()
                cheatsDir.listFiles()
                    ?.filter { it.isFile && CheatFilePattern.matches(it.name) }
                    ?.sortedBy { it.name.lowercase(Locale.ROOT) }
                    ?.forEach { file ->
                        val disabled = disabledNames(file)
                        parseSectionNames(file).forEach { name ->
                            result += GameCheat(
                                name = name,
                                packName = pack.name,
                                buildId = file.nameWithoutExtension.uppercase(Locale.ROOT),
                                file = file,
                                enabled = name !in disabled,
                                packEnabled = packEnabled
                            )
                        }
                    }
            }
        return result
    }

    fun setEnabled(cheat: GameCheat, enabled: Boolean) {
        val disabled = disabledNames(cheat.file).toMutableSet()
        if (enabled)
            disabled.remove(cheat.name)
        else
            disabled.add(cheat.name)
        writeDisabled(cheat.file, disabled)
    }

    fun remove(cheat: GameCheat) {
        val file = cheat.file
        if (!file.isFile)
            return

        val lines = file.readLines()
        val output = mutableListOf<String>()
        var removing = false
        var removed = false

        for (line in lines) {
            val trimmed = line.trim()
            val isRegularHeader = trimmed.startsWith("[") && trimmed.endsWith("]") && trimmed.length > 2
            val isMasterHeader = trimmed.startsWith("{") && trimmed.endsWith("}") && trimmed.length > 2
            val isHeader = isRegularHeader || isMasterHeader

            if (removing && isHeader)
                removing = false

            if (!removed && isRegularHeader && trimmed.substring(1, trimmed.length - 1) == cheat.name) {
                removing = true
                removed = true
                continue
            }

            if (!removing)
                output += line
        }

        if (!removed)
            throw IllegalStateException("Failed to find cheat ${cheat.name}")

        val temp = File(file.parentFile, ".${file.name}.tmp")
        val normalized = output.joinToString("\n").trimEnd()
        if (normalized.isEmpty())
            temp.writeText("")
        else
            temp.writeText("$normalized\n")

        if (file.exists() && !file.delete()) {
            temp.delete()
            throw IllegalStateException("Failed to delete cheat ${cheat.name}")
        }
        if (!temp.renameTo(file)) {
            temp.copyTo(file, overwrite = true)
            if (!temp.delete())
                throw IllegalStateException("Failed to finalize cheat deletion")
        }

        val disabled = disabledNames(file).toMutableSet()
        disabled.remove(cheat.name)
        writeDisabled(file, disabled)

        if (parseSectionNames(file).isEmpty()) {
            val stateFile = disabledFile(file)
            if (file.exists() && !file.delete())
                throw IllegalStateException("Failed to delete empty cheat file")
            if (stateFile.exists() && !stateFile.delete())
                throw IllegalStateException("Failed to delete cheat state")

            val cheatsDir = file.parentFile
            if (cheatsDir?.isDirectory == true && cheatsDir.listFiles().isNullOrEmpty())
                cheatsDir.delete()
        }
    }

    private fun parseSectionNames(file: File): List<String> {
        val names = mutableListOf<String>()
        var currentName: String? = null
        var hasOpcodes = false

        fun flush() {
            val name = currentName
            if (name != null && hasOpcodes)
                names += name
        }

        file.useLines { lines ->
            lines.forEach { rawLine ->
                val line = rawLine.trim()
                when {
                    line.startsWith("[") && line.endsWith("]") && line.length > 2 -> {
                        flush()
                        currentName = line.substring(1, line.length - 1)
                        hasOpcodes = false
                    }
                    line.startsWith("{") && line.endsWith("}") -> {
                        flush()
                        currentName = null // Master codes are always handled by the native VM.
                        hasOpcodes = false
                    }
                    currentName != null && line.isNotEmpty() -> {
                        val words = line.split(Whitespace).filter { it.isNotEmpty() }
                        if (words.isNotEmpty() && words.all { OpcodeWord.matches(it) })
                            hasOpcodes = true
                    }
                }
            }
        }
        flush()
        return names
    }

    private fun disabledNames(file: File): Set<String> {
        val stateFile = disabledFile(file)
        if (!stateFile.isFile)
            return emptySet()
        return stateFile.readLines().map { it.removeSuffix("\r") }.filter { it.isNotEmpty() }.toSet()
    }

    private fun writeDisabled(file: File, disabled: Set<String>) {
        val target = disabledFile(file)
        if (disabled.isEmpty()) {
            if (target.exists() && !target.delete())
                throw IllegalStateException("Failed to update cheat state")
            return
        }

        val temp = File(target.parentFile, ".${target.name}.tmp")
        temp.writeText(disabled.sorted().joinToString("\n", postfix = "\n"))
        if (target.exists() && !target.delete()) {
            temp.delete()
            throw IllegalStateException("Failed to update cheat state")
        }
        if (!temp.renameTo(target)) {
            temp.copyTo(target, overwrite = true)
            if (!temp.delete())
                throw IllegalStateException("Failed to finalize cheat state")
        }
    }

    private fun disabledFile(file: File): File {
        return File(file.parentFile, "${file.nameWithoutExtension}.disabled")
    }

    private fun findChildDirectory(parent: File, name: String): File? {
        return parent.listFiles()?.firstOrNull {
            it.isDirectory && it.name.equals(name, ignoreCase = true)
        }
    }

    companion object {
        private val CheatFilePattern = Regex("^[0-9A-Fa-f]{16}\\.txt$")
        private val OpcodeWord = Regex("^[0-9A-Fa-f]{8}$")
        private val Whitespace = Regex("\\s+")
    }
}
