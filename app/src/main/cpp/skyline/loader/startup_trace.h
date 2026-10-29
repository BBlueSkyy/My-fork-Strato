// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace skyline::loader {
    // Startup only: the synchronous logcat copy survives loss of the async log
    // queue during a native crash. This deliberately installs no signal handler.
    class StartupTrace {
        std::string stage;
        std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
        int exceptions{std::uncaught_exceptions()};
        bool completed{};

        void Write(const char *status, bool error = false) const noexcept {
            const auto elapsed{std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()};
#ifdef __ANDROID__
            __android_log_print(error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "StratoStartup",
                                "stage=%s status=%s us=%lld", stage.c_str(), status, static_cast<long long>(elapsed));
#endif
            try {
                if (error)
                    LOGE("Startup: stage={} status={} us={}", stage, status, elapsed);
                else
                    LOGI("Startup: stage={} status={} us={}", stage, status, elapsed);
            } catch (...) {
                // Diagnostics must not replace a loader failure while unwinding.
            }
        }

      public:
        explicit StartupTrace(std::string_view stage) : stage(stage) { Write("enter"); }
        StartupTrace(const StartupTrace &) = delete;
        StartupTrace &operator=(const StartupTrace &) = delete;
        void Complete() noexcept {
            if (!completed) {
                completed = true;
                Write("ok");
            }
        }
        ~StartupTrace() {
            if (!completed)
                Write(std::uncaught_exceptions() > exceptions ? "unwind" : "incomplete", true);
        }
    };
}
