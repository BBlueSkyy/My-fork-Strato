/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.app.Activity
import android.content.Intent
import android.graphics.BitmapFactory
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Process
import android.os.RemoteException
import android.view.ViewGroup
import android.widget.ImageView

/**
 * Lives in a dedicated Android process so it can keep the transition frame visible while the
 * emulation process is killed and recreated from a completely clean native state.
 */
class ProgramRelaunchActivity : Activity() {
    companion object {
        const val TargetIntentTag = "programRelaunchTargetIntent"
        const val SnapshotPathTag = "programRelaunchSnapshotPath"
        const val OldProcessIdTag = "programRelaunchOldProcessId"
        const val ProcessDeathTokenBundleTag = "programRelaunchDeathTokenBundle"
        const val ProcessDeathTokenTag = "programRelaunchDeathToken"
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private var relaunchStarted = false
    private var launchCompleted = false

    override fun onCreate(savedInstanceState : Bundle?) {
        super.onCreate(savedInstanceState)
        ProgramRelaunchTrace.write(this, "trampoline_created task_id=$taskId task_root=$isTaskRoot")

        window.statusBarColor = Color.BLACK
        window.navigationBarColor = Color.BLACK

        val imageView = ImageView(this).apply {
            setBackgroundColor(Color.BLACK)
            scaleType = ImageView.ScaleType.FIT_CENTER
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        }

        intent.getStringExtra(SnapshotPathTag)?.let { path ->
            BitmapFactory.decodeFile(path)?.let(imageView::setImageBitmap)
        }
        setContentView(imageView)

        // Do not kill the old emulation process until this window has submitted at least one draw.
        imageView.viewTreeObserver.addOnDrawListener {
            if (!relaunchStarted) {
                relaunchStarted = true
                imageView.post(::relaunch)
            }
        }
    }

    private fun relaunch() {
        val targetIntent = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(TargetIntentTag, Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra<Intent>(TargetIntentTag)
        } ?: run {
            ProgramRelaunchTrace.write(this, "trampoline_missing_target_intent")
            finish()
            return
        }

        val deathToken = intent.getBundleExtra(ProcessDeathTokenBundleTag)?.getBinder(ProcessDeathTokenTag)
        val oldPid = intent.getIntExtra(OldProcessIdTag, -1)
        if (deathToken == null || oldPid <= 0 || oldPid == Process.myPid()) {
            ProgramRelaunchTrace.write(this, "trampoline_invalid_handoff token=${deathToken != null} old_pid=$oldPid self=${Process.myPid()}")
            finish()
            return
        }

        val deathRecipient = IBinder.DeathRecipient {
            ProgramRelaunchTrace.write(this, "binder_died old_pid=$oldPid")
            mainHandler.post {
                launchTarget(targetIntent)
            }
        }

        try {
            deathToken.linkToDeath(deathRecipient, 0)
            ProgramRelaunchTrace.write(this, "death_recipient_registered old_pid=$oldPid")
        } catch (_ : RemoteException) {
            ProgramRelaunchTrace.write(this, "death_token_already_dead old_pid=$oldPid")
            launchTarget(targetIntent)
            return
        }

        ProgramRelaunchTrace.write(this, "old_process_kill_requested old_pid=$oldPid")
        Process.killProcess(oldPid)
    }

    private fun launchTarget(targetIntent : Intent) {
        if (launchCompleted)
            return
        launchCompleted = true

        targetIntent.setClass(this, EmulationActivity::class.java)
        targetIntent.flags = targetIntent.flags and
            (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)
        targetIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
        ProgramRelaunchTrace.write(this, "new_emulation_activity_requested from_task=$taskId")
        startActivity(targetIntent)
        overridePendingTransition(0, 0)
        finish()
    }
}
