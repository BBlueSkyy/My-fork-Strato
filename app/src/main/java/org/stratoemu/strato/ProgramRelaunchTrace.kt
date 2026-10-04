/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.content.Context
import android.os.Process
import android.os.SystemClock
import java.io.File
import java.io.FileOutputStream

object ProgramRelaunchTrace {
    private const val FileName = "program_relaunch.log"

    private fun file(context : Context) : File =
        File(context.getPublicFilesDir(), "logs/$FileName")

    @Synchronized
    fun reset(context : Context) {
        val file = file(context)
        file.parentFile?.mkdirs()
        file.writeText("")
        write(context, "trace_reset")
    }

    @Synchronized
    fun write(context : Context, event : String) {
        try {
            val file = file(context)
            file.parentFile?.mkdirs()
            val line = "${SystemClock.elapsedRealtime()} pid=${Process.myPid()} tid=${Process.myTid()} $event\n"
            FileOutputStream(file, true).use { output ->
                output.write(line.toByteArray(Charsets.UTF_8))
                output.fd.sync()
            }
        } catch (_ : Exception) {
            // Diagnostic logging must never affect emulation or relaunch behavior.
        }
    }
}
