/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.app.Service
import android.content.Intent
import android.os.Binder
import android.os.IBinder

/**
 * Starts the fresh emulation Android process before EmulationActivity is launched.
 * Keeping the process warm avoids exposing Android's cold-start window during the
 * cross-process multiprogram handoff.
 */
class ProgramRelaunchPrewarmService : Service() {
    private val binder = Binder()

    override fun onBind(intent : Intent?) : IBinder = binder
}
