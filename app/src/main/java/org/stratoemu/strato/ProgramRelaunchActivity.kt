/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.app.Activity
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.ActivityInfo
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
            unbindPrewarmService()
            finish()
            overridePendingTransition(0, 0)
        }
    }
    private var prewarmBound = false
    private var pendingTargetIntent : Intent? = null
    private val prewarmConnection = object : ServiceConnection {
        override fun onServiceConnected(name : ComponentName?, service : IBinder?) {
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
        targetIntent = readTargetIntent()
        val orientationPolicy = targetIntent?.getIntExtra(
            EmulationActivity.ProgramRelaunchOrientationPolicyTag,
            ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        ) ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED

        if (orientationPolicy != ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED)
            requestedOrientation = orientationPolicy

        super.onCreate(savedInstanceState)

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

        val binding = ProgramRelaunchLoadingBinding.inflate(layoutInflater)

        val item = try {
            targetIntent?.serializable<BaseAppItem>(AppItemTag)
        } catch (_ : Exception) {
            null
        }
        val snapshotPath = targetIntent?.getStringExtra(EmulationActivity.ProgramRelaunchSnapshotPathTag)

        try {
            ProgramRelaunchUi.configure(
                binding,
                item,
                snapshotPath = snapshotPath
            )
        } catch (_ : Exception) {
            ProgramRelaunchUi.configure(
                binding,
                null,
                snapshotPath = snapshotPath
            )
        }

        setContentView(binding.root)

        // Do not terminate the old process before the loading window has drawn.
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
            finish()
            return
        }

        val deathToken = intent.getBundleExtra(ProcessDeathTokenBundleTag)?.getBinder(ProcessDeathTokenTag)
        val oldPid = intent.getIntExtra(OldProcessIdTag, -1)
        if (deathToken == null || oldPid <= 0 || oldPid == Process.myPid()) {
            finish()
            return
        }

        val deathRecipient = IBinder.DeathRecipient {
            oldProcessDead = true
            mainHandler.post {
                launchTarget(targetIntent)
            }
        }

        try {
            deathToken.linkToDeath(deathRecipient, 0)
        } catch (_ : RemoteException) {
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

        oldActivityFinishReceiver.send(FinishOldActivityRequest, null)
    }

    private fun killOldProcess(oldPid : Int) {
        if (oldProcessDead)
            return
        Process.killProcess(oldPid)
    }

    private fun launchTarget(targetIntent : Intent) {
        if (launchCompleted || pendingTargetIntent != null)
            return

        pendingTargetIntent = targetIntent
        val prewarmIntent = Intent(this, ProgramRelaunchPrewarmService::class.java)
        prewarmBound = try {
            bindService(prewarmIntent, prewarmConnection, Context.BIND_AUTO_CREATE)
        } catch (_ : RuntimeException) {
            false
        }

        if (!prewarmBound)
            launchTargetNow(targetIntent)
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
        targetIntent.putExtra(FirstFrameReadyReceiverTag, firstFrameReadyReceiver)
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
