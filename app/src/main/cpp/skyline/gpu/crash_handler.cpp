// crash_handler.cpp
//
// See crash_handler.h for usage instructions and symbol resolution
// (addr2line) instructions at the end of this file.

#include "crash_handler.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <unwind.h>

namespace skyline::crash {

    namespace {

        constexpr int kMaxFrames = 64;
        constexpr const char *kLogFileName = "strato_native_crash.log";

        // Static buffer for the log file path, built in Install() (we can't
        // dynamically allocate memory inside the signal handler).
        char gLogPath[512] = {};
        bool gHavePath = false;

        // Simple flag to avoid reentrancy in case the process crashes again
        // inside the handler itself.
        std::atomic_flag gHandling = ATOMIC_FLAG_INIT;

        struct BacktraceState {
            void **current;
            void **end;
        };

        // libunwind/libgcc callback used by _Unwind_Backtrace to fill the
        // array of return addresses.
        _Unwind_Reason_Code UnwindCallback(struct _Unwind_Context *context, void *arg) {
            auto *state{static_cast<BacktraceState *>(arg)};
            if (state->current == state->end)
                return _URC_END_OF_STACK;

            if (uintptr_t pc{_Unwind_GetIP(context)})
                *state->current++ = reinterpret_cast<void *>(pc);

            return _URC_NO_REASON;
        }

        // --- Async-signal-safe helpers (no malloc, no buffered stdio) ---

        void SafeWrite(int fd, const char *str) {
            write(fd, str, strlen(str));
        }

        void SafeWriteHex(int fd, uintptr_t value) {
            char buf[2 + sizeof(uintptr_t) * 2 + 1];
            char *p{buf + sizeof(buf) - 1};
            *p = '\0';

            static const char digits[]{"0123456789abcdef"};
            if (value == 0) {
                *--p = '0';
            } else {
                while (value) {
                    *--p = digits[value & 0xF];
                    value >>= 4;
                }
            }
            *--p = 'x';
            *--p = '0';
            write(fd, p, strlen(p));
        }

        void SafeWriteDec(int fd, long value) {
            char buf[24];
            char *p{buf + sizeof(buf) - 1};
            *p = '\0';

            bool negative{value < 0};
            unsigned long v{negative ? static_cast<unsigned long>(-value) : static_cast<unsigned long>(value)};

            if (v == 0) {
                *--p = '0';
            } else {
                while (v) {
                    *--p = static_cast<char>('0' + (v % 10));
                    v /= 10;
                }
            }
            if (negative)
                *--p = '-';
            write(fd, p, strlen(p));
        }

        const char *SignalName(int sig) {
            switch (sig) {
                case SIGSEGV: return "SIGSEGV";
                case SIGABRT: return "SIGABRT";
                case SIGILL:  return "SIGILL";
                case SIGBUS:  return "SIGBUS";
                case SIGFPE:  return "SIGFPE";
                default:      return "UNKNOWN_SIGNAL";
            }
        }

        void DumpMaps(int fd) {
            int mapsFd{open("/proc/self/maps", O_RDONLY)};
            if (mapsFd < 0)
                return;

            char buf[512];
            ssize_t n;
            while ((n = read(mapsFd, buf, sizeof(buf))) > 0)
                write(fd, buf, static_cast<size_t>(n));

            close(mapsFd);
        }

        void HandleSignal(int sig, siginfo_t *info, void * /*ucontext*/) {
            // Avoid reentrancy: if it crashes again inside the handler, die
            // immediately instead of looping.
            if (gHandling.test_and_set())
                _exit(128 + sig);

            int fd{-1};
            if (gHavePath)
                fd = open(gLogPath, O_CREAT | O_WRONLY | O_APPEND, 0644);
            if (fd < 0)
                fd = STDERR_FILENO;

            SafeWrite(fd, "\n=== Strato: native crash captured ===\n");
            SafeWrite(fd, "signal: ");
            SafeWrite(fd, SignalName(sig));
            SafeWrite(fd, "  tid: ");
            SafeWriteDec(fd, static_cast<long>(gettid()));
            SafeWrite(fd, "  fault address: ");
            SafeWriteHex(fd, reinterpret_cast<uintptr_t>(info->si_addr));
            SafeWrite(fd, "\n\nbacktrace (raw addresses - resolve with addr2line, see note at end of .cpp):\n");

            void *frames[kMaxFrames];
            BacktraceState state{frames, frames + kMaxFrames};
            _Unwind_Backtrace(UnwindCallback, &state);
            auto frameCount{static_cast<size_t>(state.current - frames)};

            for (size_t i{}; i < frameCount; i++) {
                SafeWrite(fd, "  #");
                SafeWriteDec(fd, static_cast<long>(i));
                SafeWrite(fd, " pc ");
                SafeWriteHex(fd, reinterpret_cast<uintptr_t>(frames[i]));
                SafeWrite(fd, "\n");
            }

            SafeWrite(fd, "\n--- /proc/self/maps (to find each .so's base address and compute the offset) ---\n");
            DumpMaps(fd);
            SafeWrite(fd, "=== end of dump ===\n");

            if (fd != STDERR_FILENO)
                close(fd);

            // Restore the default handler and re-raise the signal: the
            // process still dies the normal way (and if Android manages to
            // generate a tombstone, it's also created - duplicate dump,
            // no harm done).
            signal(sig, SIG_DFL);
            raise(sig);
        }

    }

    void Install(const std::string &logDirectory) {
        if (!logDirectory.empty()) {
            std::string path{logDirectory + "/" + kLogFileName};
            if (path.size() < sizeof(gLogPath)) {
                strncpy(gLogPath, path.c_str(), sizeof(gLogPath) - 1);
                gHavePath = true;
            }
        }

        struct sigaction sa{};
        sa.sa_sigaction = HandleSignal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);

        // Alternate stack: without this, a crash caused by stack overflow
        // wouldn't even be able to call the handler (no stack left for it
        // to run on).
        static char altStack[SIGSTKSZ * 4];
        stack_t ss{};
        ss.ss_sp = altStack;
        ss.ss_size = sizeof(altStack);
        ss.ss_flags = 0;
        sigaltstack(&ss, nullptr);

        for (int sig : {SIGSEGV, SIGABRT, SIGILL, SIGBUS, SIGFPE})
            sigaction(sig, &sa, nullptr);
    }

}

// ---------------------------------------------------------------------------
// Resolving backtrace addresses (via Termux, no PC required)
// ---------------------------------------------------------------------------
//
// The dump only records raw addresses (e.g. 0x7f8a1b2c40) because it isn't
// safe to call dladdr()/malloc() inside a signal handler to resolve function
// names on the spot. Resolve this afterwards, offline:
//
// 1. In the dump, find which library each "pc" falls into by looking at the
//    /proc/self/maps block: each line has a range like
//        7f8a1a000000-7f8a1b400000 r-xp ... /data/app/.../libskyline.so
//    If the "pc" falls within that range, subtract the line's BASE address
//    (the first number, e.g. 7f8a1a000000) from the "pc" -> that's the offset.
//
// 2. Copy the NON-STRIPPED version of the .so (the one Gradle generates in
//    app/build/intermediates/.../obj/<abi>/libskyline.so before packaging,
//    not the one that ends up inside the final APK) to your Termux environment.
//
// 3. For each offset, run:
//        aarch64-linux-android-addr2line -f -C -e libskyline.so <offset>
//    (the addr2line binary comes with the NDK; if you don't have the full
//    NDK in Termux, Termux's `llvm-addr2line` from the `llvm` package works
//    the same way, just swap the binary name).
//
// This gives you the function name and file:line for each frame of the crash.
