// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "readback_probe.h"
#if STRATO_READBACK_DIAGNOSTICS
#include <condition_variable>
#include <mutex>
#include <thread>
#endif
namespace skyline::readback_probe {
    class Session {
#if STRATO_READBACK_DIAGNOSTICS
        std::mutex mutex;
        std::condition_variable condition;
        bool stopping{};
        std::thread reporter;
#endif
      public:
        Session(bool fastReadback, bool fastWrites, bool maxClocks, bool freeGuest);
        void Stop();
        ~Session();
    };
} // namespace skyline::readback_probe
