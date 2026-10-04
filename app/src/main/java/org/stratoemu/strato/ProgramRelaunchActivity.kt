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
import android.os.Process
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
    }

    private var relaunchStarted = false

    override fun onCreate(savedInstanceState : Bundle?) {
        super.onCreate(savedInstanceState)

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
            intent.getParcelableExtra(TargetIntentTag)
        } ?: run {
            finish()
            return
        }

        val oldPid = intent.getIntExtra(OldProcessIdTag, -1)
        if (oldPid > 0 && oldPid != Process.myPid())
            Process.killProcess(oldPid)

        targetIntent.setClass(this, EmulationActivity::class.java)
        targetIntent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        startActivity(targetIntent)
        overridePendingTransition(0, 0)
        finish()
    }
}
