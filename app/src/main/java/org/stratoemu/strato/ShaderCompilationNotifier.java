/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato;

import android.app.Activity;
import android.view.View;
import android.widget.TextView;

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

    private static final class State {
        final TextView notice;
        final Runnable hideRunnable;
        int pendingCompilations;
        int compilationCount;

        State(Activity activity) {
            notice = activity.findViewById(R.id.shader_compilation_stats);
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
