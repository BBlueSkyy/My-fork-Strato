/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato;

import android.app.Activity;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import androidx.core.content.ContextCompat;

import java.util.WeakHashMap;

public final class ShaderCompilationNotifier {
    private static final long HideDelayMs = 1000;
    private static final WeakHashMap<Activity, State> States = new WeakHashMap<>();

    private ShaderCompilationNotifier() {}

    public static void update(Activity activity, boolean compiling) {
        activity.runOnUiThread(() -> {
            if (activity.isFinishing() || activity.isDestroyed())
                return;

            State state = States.get(activity);
            if (state == null) {
                state = new State(activity);
                States.put(activity, state);
            }

            state.notice.removeCallbacks(state.hideRunnable);

            if (compiling) {
                state.pendingCompilations++;
                state.compilationCount++;
                state.updateText(activity);
                state.notice.setVisibility(View.VISIBLE);
            } else {
                state.pendingCompilations = Math.max(0, state.pendingCompilations - 1);
                if (state.pendingCompilations == 0)
                    state.notice.postDelayed(state.hideRunnable, HideDelayMs);
            }
        });
    }

    private static int dp(Activity activity, int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }

    private static final class State {
        final TextView notice;
        final Runnable hideRunnable;
        int pendingCompilations;
        int compilationCount;

        State(Activity activity) {
            notice = new TextView(activity);
            notice.setTextColor(ContextCompat.getColor(activity, R.color.colorPerfStatsPrimary));
            notice.setTextSize(14);
            notice.setGravity(Gravity.CENTER);
            notice.setPadding(dp(activity, 12), dp(activity, 7), dp(activity, 12), dp(activity, 7));

            GradientDrawable background = new GradientDrawable();
            background.setColor(0xB3000000);
            background.setCornerRadius(dp(activity, 8));
            notice.setBackground(background);
            notice.setElevation(dp(activity, 4));
            notice.setVisibility(View.GONE);

            FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.BOTTOM | Gravity.END);
            params.setMarginEnd(dp(activity, 20));
            params.bottomMargin = dp(activity, 20);

            ViewGroup content = activity.findViewById(android.R.id.content);
            content.addView(notice, params);

            hideRunnable = () -> {
                if (pendingCompilations == 0)
                    notice.setVisibility(View.GONE);
            };
        }

        void updateText(Activity activity) {
            notice.setText(activity.getString(R.string.compiling_shaders, compilationCount));
        }
    }
}
