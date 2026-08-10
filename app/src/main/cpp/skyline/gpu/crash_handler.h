// crash_handler.h
//
// Native crash handler to catch SIGSEGV/SIGABRT/SIGILL/SIGBUS/SIGFPE and
// write a dump (signal, thread, raw backtrace, /proc/self/maps) directly to
// the app's storage, without depending on `adb logcat`/tombstone.
//
// Usage:
//   1. Call skyline::crash::Install(path) as early as possible during the
//      app's native initialization (e.g. JNI_OnLoad or the point where
//      emulation starts), passing a directory the process already has
//      write permission to (e.g. context.getExternalFilesDir(null) from
//      the Kotlin side, forwarded via JNI).
//   2. If the app crashes, the file "strato_native_crash.log" will appear
//      in that directory with the dump.
//   3. Resolve the backtrace addresses offline with addr2line (can be run
//      in Termux, see instructions at the end of the .cpp).

#pragma once

#include <string>

namespace skyline::crash {

    /**
     * Installs the signal handlers. Must be called exactly once, as early
     * as possible in the process's lifetime.
     *
     * @param logDirectory Writable directory where the crash file will be
     *        created. If empty, falls back to stderr (visible in logcat,
     *        if available).
     */
    void Install(const std::string &logDirectory = "");

}
