// SPDX-License-Identifier: MPL-2.0
// Temporary, opt-in crash diagnosis for My Hero One's Justice (010039000B68E000).
// No signal handlers are installed here: the existing NCE/trap chain owns signals.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <dlfcn.h>
#include <fcntl.h>
#include <string>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

namespace skyline::diagnostics::myhero {
    enum class Event : std::uint32_t {
        Begin, OsExecute, LoaderReady, GpuReady, GuestStart, GuestEnd,
        Surface, StopRequest, CommandRecord, CommandSubmit,
        BufferAlloc, BufferFree, ImageAlloc, ImageFree,
        TextureSync, TextureDestroy, NativeException, SignalFrame
    };

    struct Entry {
        std::atomic<std::uint64_t> sequence{0};
        std::atomic<std::uint64_t> timeMs{0};
        std::atomic<std::uint64_t> first{0};
        std::atomic<std::uint64_t> second{0};
        std::atomic<std::uint32_t> event{0};
    };

    // Fixed-size, allocation-free event ring. All fields are atomic so the signal
    // handler can read entries without racing Vulkan/guest worker threads.
    inline constexpr std::size_t Capacity{512};
    inline std::array<Entry, Capacity> ring{};
    inline std::atomic<std::uint64_t> sequence{0};
    inline std::atomic<int> outputFd{-1};
    inline std::atomic<std::uint64_t> lastSnapshotSecond{0};
    inline std::atomic<unsigned> periodicSnapshots{0};
    inline std::atomic<std::uint64_t> pakReadCount{0};
    inline std::atomic<std::uint64_t> pakSampleCount{0};
    inline std::atomic<std::uint64_t> romfsReadCount{0};
    inline std::atomic<std::uint64_t> romfsSampleCount{0};
    inline std::atomic<std::uint64_t> romfsErrorCount{0};
    inline std::atomic_flag dumping = ATOMIC_FLAG_INIT;

    inline bool Enabled() noexcept {
        return outputFd.load(std::memory_order_relaxed) >= 0;
    }

    inline std::uint64_t MonotonicMs() noexcept {
        struct timespec t{};
        if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
            return 0;
        return static_cast<std::uint64_t>(t.tv_sec) * 1000 + static_cast<std::uint64_t>(t.tv_nsec) / 1000000;
    }

    struct Line {
        char data[256]{};
        std::size_t size{};

        void Add(const char *str) noexcept {
            if (!str)
                return;
            while (*str && size < sizeof(data) - 2)
                data[size++] = *str++;
        }

        void Decimal(std::uint64_t number) noexcept {
            char digits[24];
            std::size_t count{};
            do {
                digits[count++] = static_cast<char>('0' + number % 10);
                number /= 10;
            } while (number && count < sizeof(digits));
            while (count && size < sizeof(data) - 2)
                data[size++] = digits[--count];
        }

        void Hex(std::uint64_t number) noexcept {
            Add("0x");
            constexpr char Digits[]{"0123456789abcdef"};
            bool found{};
            for (int shift = 60; shift >= 0; shift -= 4) {
                const auto digit = (number >> shift) & 0xf;
                if (digit || found || !shift) {
                    found = true;
                    if (size < sizeof(data) - 2)
                        data[size++] = Digits[digit];
                }
            }
        }

        void Write() noexcept {
            const int fd{outputFd.load(std::memory_order_acquire)};
            if (fd < 0)
                return;
            data[size++] = '\n';
            // A single O_APPEND write per line; do not use stdio, fmt, mutexes,
            // malloc, unwinding, fsync, or stack walking from a signal context.
            (void)write(fd, data, size);
        }
    };

    inline const char *EventName(Event event) noexcept {
        switch (event) {
            case Event::Begin: return "begin";
            case Event::OsExecute: return "os_execute";
            case Event::LoaderReady: return "loader_ready";
            case Event::GpuReady: return "gpu_ready";
            case Event::GuestStart: return "guest_start";
            case Event::GuestEnd: return "guest_end";
            case Event::Surface: return "surface";
            case Event::StopRequest: return "stop_request";
            case Event::CommandRecord: return "cmd_record";
            case Event::CommandSubmit: return "cmd_submit";
            case Event::BufferAlloc: return "buffer_alloc";
            case Event::BufferFree: return "buffer_free";
            case Event::ImageAlloc: return "image_alloc";
            case Event::ImageFree: return "image_free";
            case Event::TextureSync: return "texture_sync";
            case Event::TextureDestroy: return "texture_destroy";
            case Event::NativeException: return "native_exception";
            case Event::SignalFrame: return "signal_frame";
        }
        return "unknown";
    }

    inline void Mark(Event event, std::uint64_t first = 0, std::uint64_t second = 0) noexcept {
        if (!Enabled())
            return;
        const auto id{sequence.fetch_add(1, std::memory_order_relaxed) + 1};
        auto &slot{ring[(id - 1) % Capacity]};
        slot.sequence.store(0, std::memory_order_relaxed);
        slot.timeMs.store(MonotonicMs(), std::memory_order_relaxed);
        slot.first.store(first, std::memory_order_relaxed);
        slot.second.store(second, std::memory_order_relaxed);
        slot.event.store(static_cast<std::uint32_t>(event), std::memory_order_relaxed);
        slot.sequence.store(id, std::memory_order_release);
    }

    // Also used by the fatal signal path. Bounded; no allocations or waits.
    inline void Snapshot(const char *reason) noexcept {
        if (!Enabled() || dumping.test_and_set(std::memory_order_acquire))
            return;
        const auto last{sequence.load(std::memory_order_acquire)};
        Line header;
        header.Add("SNAPSHOT ");
        header.Add(reason);
        header.Add(" uptime_ms=");
        header.Decimal(MonotonicMs());
        header.Add(" last_seq=");
        header.Decimal(last);
        header.Write();

        const auto first{last > 128 ? last - 127 : 1};
        for (std::uint64_t id = first; id <= last; ++id) {
            const auto &slot{ring[(id - 1) % Capacity]};
            if (slot.sequence.load(std::memory_order_acquire) != id)
                continue;
            const auto timestamp{slot.timeMs.load(std::memory_order_relaxed)};
            const auto event{slot.event.load(std::memory_order_relaxed)};
            const auto a{slot.first.load(std::memory_order_relaxed)};
            const auto b{slot.second.load(std::memory_order_relaxed)};
            if (slot.sequence.load(std::memory_order_acquire) != id)
                continue;
            Line line;
            line.Add("event seq=");
            line.Decimal(id);
            line.Add(" ms=");
            line.Decimal(timestamp);
            line.Add(" type=");
            line.Add(EventName(static_cast<Event>(event)));
            line.Add(" a=");
            line.Hex(a);
            line.Add(" b=");
            line.Hex(b);
            line.Write();
        }
        dumping.clear(std::memory_order_release);
    }

    // Emit the latest ring periodically so a hard process death without a
    // userspace signal callback still leaves useful pre-crash events on disk.
    inline void PeriodicSnapshot() noexcept {
        if (!Enabled() || periodicSnapshots.load(std::memory_order_relaxed) >= 300)
            return;
        const auto now{MonotonicMs() / 1000};
        auto previous{lastSnapshotSecond.load(std::memory_order_relaxed)};
        if (now < previous + 3 ||
            !lastSnapshotSecond.compare_exchange_strong(previous, now, std::memory_order_relaxed))
            return;
        periodicSnapshots.fetch_add(1, std::memory_order_relaxed);
        Snapshot("periodic");
    }

    // Called on the guest's file-read service path, not within a signal handler.
    // Samples one in 64 reads of .pak files (max 1500) and all read errors.
    // A hash of at most 4 KiB is diagnostic, not a complete integrity check.
    inline void PakRead(const char *path, std::uint64_t offset,
                        std::uint64_t requested, std::uint64_t actual,
                        int error, const std::uint8_t *data, std::size_t dataSize) noexcept {
        if (!Enabled())
            return;
        const auto count{pakReadCount.fetch_add(1, std::memory_order_relaxed) + 1};
        if (!error && count % 64 != 0)
            return;
        if (!error && pakSampleCount.fetch_add(1, std::memory_order_relaxed) >= 1500)
            return;

        const auto hashLength{data && !error ? std::min<std::size_t>(dataSize, 4096) : 0};
        std::uint64_t hash{0xcbf29ce484222325ULL};
        for (std::size_t index{}; index < hashLength; ++index)
            hash = (hash ^ data[index]) * 0x100000001b3ULL;

        Line line;
        line.Add("PAK_READ ms=");
        line.Decimal(MonotonicMs());
        line.Add(" count=");
        line.Decimal(count);
        line.Add(" offset=");
        line.Hex(offset);
        line.Add(" requested=");
        line.Decimal(requested);
        line.Add(" actual=");
        line.Decimal(actual);
        line.Add(" error=");
        line.Decimal(static_cast<std::uint64_t>(error));
        line.Add(" hash_first4k=");
        line.Hex(hash);
        line.Add(" hashed=");
        line.Decimal(hashLength);
        line.Add(" path=");
        line.Add(path);
        line.Write();
    }

    // The game mounts its current-process RomFS through IStorage, not IFile.
    // Sample every 32nd read, capped at 3000, plus all errors and short reads.
    // The hashes cover only the returned first/last 4 KiB, not full RomFS data.
    inline void RomfsRead(std::uint64_t offset, std::size_t requested,
                          std::size_t actual, int error,
                          const std::uint8_t *data, std::size_t dataSize) noexcept {
        if (!Enabled())
            return;
        const auto count{romfsReadCount.fetch_add(1, std::memory_order_relaxed) + 1};
        const bool problem{error != 0 || actual != requested};
        if (problem)
            romfsErrorCount.fetch_add(1, std::memory_order_relaxed);
        if (!problem && count % 32 != 0)
            return;
        if (!problem && romfsSampleCount.fetch_add(1, std::memory_order_relaxed) >= 3000)
            return;

        const auto bytes{data ? std::min(dataSize, actual) : std::size_t{}};
        const auto headSize{std::min<std::size_t>(bytes, 4096)};
        const auto tailSize{std::min<std::size_t>(bytes, 4096)};
        std::uint64_t head{0xcbf29ce484222325ULL};
        std::uint64_t tail{0xcbf29ce484222325ULL};
        for (std::size_t i{}; i < headSize; ++i)
            head = (head ^ data[i]) * 0x100000001b3ULL;
        for (std::size_t i = bytes - tailSize; i < bytes; ++i)
            tail = (tail ^ data[i]) * 0x100000001b3ULL;

        Line line;
        line.Add("ROMFS_READ ms=");
        line.Decimal(MonotonicMs());
        line.Add(" n=");
        line.Decimal(count);
        line.Add(" off=");
        line.Hex(offset);
        line.Add(" req=");
        line.Decimal(requested);
        line.Add(" got=");
        line.Decimal(actual);
        line.Add(" err=");
        line.Decimal(static_cast<std::uint64_t>(error));
        line.Add(" h=");
        line.Hex(head);
        line.Add(" t=");
        line.Hex(tail);
        line.Add(" hashed=");
        line.Decimal(headSize);
        line.Write();
    }

    // Called only during executable loading, never from signal context.
    // Log the real guest virtual ranges so a fault PC can be mapped to an NSO.
    inline void ExecutableMap(const char *name, std::uint64_t allocationBase,
                              std::uint64_t textBase, std::uint64_t textSize) noexcept {
        if (!Enabled())
            return;
        Line line;
        line.Add("EXECUTABLE name=");
        line.Add(name);
        line.Add(" allocation_base=");
        line.Hex(allocationBase);
        line.Add(" text_base=");
        line.Hex(textBase);
        line.Add(" text_size=");
        line.Hex(textSize);
        line.Write();
    }

    // The NCE already constructs the guest stack string. Persist the existing
    // result for this one crash; do not allocate or unwind again for the probe.
    inline void GuestStack(const std::string &trace) noexcept {
        if (!Enabled())
            return;
        Line header;
        header.Add("GUEST_STACK bytes=");
        header.Decimal(trace.size());
        header.Write();
        const int fd{outputFd.load(std::memory_order_acquire)};
        if (fd < 0)
            return;
        const auto length{trace.size() < 4096 ? trace.size() : 4096};
        if (length)
            (void)write(fd, trace.data(), length);
        (void)write(fd, "\nEND_GUEST_STACK\n", 17);
    }

    inline void FatalSignal(int sig, const siginfo_t *info, const ucontext_t *context, const char *origin) noexcept {
        // SIGINT is used to stop emulator threads; it is not a crash.
        if (!Enabled() || sig == SIGINT)
            return;
        Line line;
        line.Add("FATAL_SIGNAL origin=");
        line.Add(origin);
        line.Add(" sig=");
        line.Decimal(static_cast<std::uint64_t>(sig));
        line.Add(" tid=");
        line.Decimal(static_cast<std::uint64_t>(gettid()));
        line.Add(" fault=");
        line.Hex(info ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0);
#if defined(__aarch64__)
        if (context) {
            line.Add(" pc=");
            line.Hex(context->uc_mcontext.pc);
            line.Add(" lr=");
            line.Hex(context->uc_mcontext.regs[30]);
            line.Add(" sp=");
            line.Hex(context->uc_mcontext.sp);
        }
#else
        (void)context;
#endif
        line.Write();
#if defined(__aarch64__)
        if (context) {
            // Read the saved mcontext registers, not arbitrary guest pointers.
            // Four registers per bounded append-only write, no heap or mutex.
            for (unsigned index = 0; index < 31; index += 4) {
                Line registers;
                registers.Add("SIGNAL_REGISTERS");
                for (unsigned column = index; column < 31 && column < index + 4; ++column) {
                    registers.Add(" x");
                    registers.Decimal(column);
                    registers.Add("=");
                    registers.Hex(context->uc_mcontext.regs[column]);
                }
                registers.Write();
            }
            Line state;
            state.Add("SIGNAL_PSTATE ");
            state.Hex(context->uc_mcontext.pstate);
            state.Add(" fault_address=");
            state.Hex(context->uc_mcontext.fault_address);
            state.Write();
        }
#endif
        Snapshot("fatal");
    }

    inline void Begin(const std::string &publicFilesPath) {
        // A fresh per-run file; bounded memory and at most 300 periodic dumps
        // (~15 minutes), plus any final signal dump.
        const auto path{publicFilesPath + "logs/myhero-battle-probe.txt"};
        const int fd{open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600)};
        if (fd < 0)
            return;
        outputFd.store(fd, std::memory_order_release);
        Mark(Event::Begin, static_cast<std::uint64_t>(getpid()), 0);
        // ASLR-aware module base for PC/LR symbolication outside the device.
        // dladdr is intentionally used only at startup, never from a signal.
        Dl_info module{};
        if (dladdr(reinterpret_cast<const void *>(&Begin), &module) != 0) {
            Line line;
            line.Add("MODULE base=");
            line.Hex(reinterpret_cast<std::uintptr_t>(module.dli_fbase));
            line.Add(" file=");
            line.Add(module.dli_fname);
            line.Write();
        }
        Snapshot("startup");
    }

    inline void Finish() noexcept {
        if (!Enabled())
            return;
        Snapshot("normal_exit");
        const int fd{outputFd.exchange(-1, std::memory_order_acq_rel)};
        if (fd >= 0)
            close(fd);
    }
}
