/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.app.Activity
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.content.Context
import android.content.ComponentName
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Process
import android.os.RemoteException
import android.os.ResultReceiver
import android.view.Surface
import android.view.WindowManager
import org.stratoemu.strato.data.AppItemTag
import org.stratoemu.strato.data.BaseAppItem
import org.stratoemu.strato.databinding.ProgramRelaunchLoadingBinding
import org.stratoemu.strato.emulation.ProgramRelaunchUi
import org.stratoemu.strato.utils.serializable

/**
 * Lives in a dedicated Android process so it can keep the transition UI visible while the
 * emulation process is killed and recreated from a completely clean native state.
 */
class ProgramRelaunchActivity : Activity() {
    companion object {
        const val TargetIntentTag = "programRelaunchTargetIntent"
        const val OldProcessIdTag = "programRelaunchOldProcessId"
        const val ProcessDeathTokenBundleTag = "programRelaunchDeathTokenBundle"
        const val ProcessDeathTokenTag = "programRelaunchDeathToken"
        const val FirstFrameReadyReceiverTag = "programRelaunchFirstFrameReadyReceiver"
        const val TrampolineReadyReceiverTag = "programRelaunchTrampolineReadyReceiver"
        const val OldActivityFinishReceiverTag = "programRelaunchOldActivityFinishReceiver"
        const val FinishOldActivityRequest = 1
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val firstFrameReadyReceiver = object : ResultReceiver(mainHandler) {
        override fun onReceiveResult(resultCode : Int, resultData : Bundle?) {
            if (resultCode != RESULT_OK || !launchCompleted)
                return
            ProgramRelaunchTrace.write(this@ProgramRelaunchActivity, "new_emulation_first_frame_ready")
            unbindPrewarmService()
            finish()
            overridePendingTransition(0, 0)
        }
    }
    private var prewarmBound = false
    private var pendingTargetIntent : Intent? = null
    private val prewarmConnection = object : ServiceConnection {
        override fun onServiceConnected(name : ComponentName?, service : IBinder?) {
            ProgramRelaunchTrace.write(this@ProgramRelaunchActivity, "new_emulation_process_ready")
            pendingTargetIntent?.let(::launchTargetNow)
        }

        override fun onServiceDisconnected(name : ComponentName?) {
        }
    }
    private var relaunchStarted = false
    private var launchCompleted = false
    @Volatile
    private var oldProcessDead = false
    private var targetIntent : Intent? = null

    override fun onCreate(savedInstanceState : Bundle?) {
        super.onCreate(savedInstanceState)
        ProgramRelaunchTrace.write(this, "trampoline_created task_id=$taskId task_root=$isTaskRoot")

        window.statusBarColor = Color.BLACK
        window.navigationBarColor = Color.BLACK
        window.attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (
            android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
            )

        targetIntent = readTargetIntent()
        val orientationPolicy = targetIntent?.getIntExtra(
            EmulationActivity.ProgramRelaunchOrientationPolicyTag,
            ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        ) ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        if (orientationPolicy != ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED)
            requestedOrientation = orientationPolicy
        ProgramRelaunchTrace.write(this, "trampoline_window_setup orientation_policy=$orientationPolicy")

        // Keep window/insets manipulation out of the handoff critical path. The old
        // emulation activity is already fullscreen, and windowDisablePreview keeps it
        // visible until this activity submits its first real frame.
        ProgramRelaunchTrace.write(this, "trampoline_window_ready")

        val binding = ProgramRelaunchLoadingBinding.inflate(layoutInflater)
        ProgramRelaunchTrace.write(this, "trampoline_loading_ui_inflated")

        val item = try {
            targetIntent?.serializable<BaseAppItem>(AppItemTag)
        } catch (exception : Exception) {
            ProgramRelaunchTrace.write(this, "trampoline_item_decode_failed ${exception.javaClass.simpleName}")
            null
        }
        val snapshotPath = targetIntent?.getStringExtra(EmulationActivity.ProgramRelaunchSnapshotPathTag)

        try {
            ProgramRelaunchUi.configure(
                binding,
                item,
                snapshotPath = snapshotPath,
                transparentBackground = false
            )
        } catch (exception : Exception) {
            ProgramRelaunchTrace.write(this, "trampoline_item_ui_failed ${exception.javaClass.simpleName}")
            ProgramRelaunchUi.configure(
                binding,
                null,
                snapshotPath = snapshotPath,
                transparentBackground = false
            )
        }

        setContentView(binding.root)
        ProgramRelaunchTrace.write(this, "trampoline_loading_ui_installed item=${item != null} orientation=$handoffOrientation")

        // The trampoline is the single visible loading screen. Confirm its first real
        // draw before allowing the old native process to disappear.
        val trampolineReadyReceiver = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(TrampolineReadyReceiverTag, ResultReceiver::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra<ResultReceiver>(TrampolineReadyReceiverTag)
        }

        val observer = binding.root.viewTreeObserver
        val drawListener = object : android.view.ViewTreeObserver.OnDrawListener {
            override fun onDraw() {
                if (relaunchStarted)
                    return
                relaunchStarted = true
                binding.root.post {
                    if (observer.isAlive)
                        observer.removeOnDrawListener(this)
                    ProgramRelaunchTrace.write(this@ProgramRelaunchActivity, "trampoline_first_draw")
                    trampolineReadyReceiver?.send(RESULT_OK, null)
                    relaunch()
                }
            }
        }
        observer.addOnDrawListener(drawListener)
        binding.root.invalidate()
    }

    private fun readTargetIntent() : Intent? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(TargetIntentTag, Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra<Intent>(TargetIntentTag)
        }

    private fun relaunch() {
        val targetIntent = targetIntent ?: readTargetIntent() ?: run {
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
            oldProcessDead = true
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

        val oldActivityFinishReceiver = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(OldActivityFinishReceiverTag, ResultReceiver::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra<ResultReceiver>(OldActivityFinishReceiverTag)
        }

        if (oldActivityFinishReceiver == null) {
            killOldProcess(oldPid)
            return
        }

        ProgramRelaunchTrace.write(this, "old_emulation_activity_finish_requested")
        oldActivityFinishReceiver.send(FinishOldActivityRequest, null)
        /*
         * No external SIGKILL here. The old EmulationActivity exits its own process after
         * onDestroy, and linkToDeath above is the only signal that advances the relaunch.
         */
    }

    private fun killOldProcess(oldPid : Int) {
        if (oldProcessDead)
            return
        ProgramRelaunchTrace.write(this, "old_process_kill_requested_fallback old_pid=$oldPid")
        Process.killProcess(oldPid)
    }

    private fun launchTarget(targetIntent : Intent) {
        if (launchCompleted || pendingTargetIntent != null)
            return

        pendingTargetIntent = targetIntent
        val prewarmIntent = Intent(this, ProgramRelaunchPrewarmService::class.java)
        prewarmBound = try {
            bindService(prewarmIntent, prewarmConnection, Context.BIND_AUTO_CREATE)
        } catch (exception : RuntimeException) {
            ProgramRelaunchTrace.write(
                this,
                "new_emulation_process_prewarm_failed ${exception.javaClass.simpleName}"
            )
            false
        }

        if (prewarmBound) {
            ProgramRelaunchTrace.write(this, "new_emulation_process_prewarm_requested")
        } else {
            ProgramRelaunchTrace.write(this, "new_emulation_process_prewarm_unavailable")
            launchTargetNow(targetIntent)
        }
    }

    /**
     * Freeze the fresh EmulationActivity to the trampoline's current physical side for its
     * hidden startup. The trampoline itself remains sensor-driven and can rotate freely.
     */
    private fun currentFixedOrientation() : Int {
        val rotation = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display?.rotation ?: Surface.ROTATION_0
        } else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay.rotation
        }

        return when (rotation) {
            Surface.ROTATION_0 ->
                if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE)
                    ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                else
                    ActivityInfo.SCREEN_ORIENTATION_PORTRAIT

            Surface.ROTATION_90 ->
                if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE)
                    ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                else
                    ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT

            Surface.ROTATION_180 ->
                if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE)
                    ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE
                else
                    ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT

            Surface.ROTATION_270 ->
                if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE)
                    ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE
                else
                    ActivityInfo.SCREEN_ORIENTATION_PORTRAIT

            else -> ActivityInfo.SCREEN_ORIENTATION_LOCKED
        }
    }

    private fun launchTargetNow(targetIntent : Intent) {
        if (launchCompleted)
            return
        launchCompleted = true
        pendingTargetIntent = null

        targetIntent.setClass(this, EmulationActivity::class.java)
        targetIntent.flags = targetIntent.flags and
            (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)
        targetIntent.putExtra(
            EmulationActivity.ProgramRelaunchOrientationTag,
            currentFixedOrientation()
        )
        targetIntent.putExtra(FirstFrameReadyReceiverTag, firstFrameReadyReceiver)
        ProgramRelaunchTrace.write(
            this,
            "new_emulation_activity_requested from_task=$taskId orientation=${targetIntent.getIntExtra(EmulationActivity.ProgramRelaunchOrientationTag, ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED)}"
        )
        startActivity(targetIntent)
        overridePendingTransition(0, 0)
    }

    private fun unbindPrewarmService() {
        if (!prewarmBound)
            return
        prewarmBound = false
        try {
            unbindService(prewarmConnection)
        } catch (_ : IllegalArgumentException) {
        }
    }

    override fun onDestroy() {
        unbindPrewarmService()
        super.onDestroy()
    }
}
