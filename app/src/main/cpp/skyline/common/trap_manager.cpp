// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2024 Strato Team and Contributors (https://github.com/strato-emu/)

#include <sys/mman.h>
#include <atomic>
#include <array>
#include <charconv>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include "trace.h"
#include "trap_manager.h"

namespace skyline {
    CallbackEntry::CallbackEntry(TrapProtection protection, LockCallback lockCallback, TrapCallback readCallback, TrapCallback writeCallback, std::weak_ptr<void> owner) : protection{protection}, lockCallback{std::move(lockCallback)}, readCallback{std::move(readCallback)}, writeCallback{std::move(writeCallback)}, owner{std::move(owner)} {}

    constexpr TrapHandle::TrapHandle(const TrapMap::GroupHandle &handle) : TrapMap::GroupHandle(handle) {}

    TrapHandle TrapManager::CreateTrap(span<span<u8>> regions, const LockCallback &lockCallback, const TrapCallback &readCallback, const TrapCallback &writeCallback, std::weak_ptr<void> owner) {
        TRACE_EVENT("host", "TrapManager::CreateTrap");
        std::scoped_lock lock{trapMutex};
        TrapHandle handle{trapMap.Insert(regions, CallbackEntry{TrapProtection::None, lockCallback, readCallback, writeCallback, std::move(owner)})};
        return handle;
    }

    void TrapManager::TrapRegions(TrapHandle handle, bool writeOnly) {
        TRACE_EVENT("host", "TrapManager::TrapRegions");
        std::scoped_lock lock{trapMutex};
        auto protection{writeOnly ? TrapProtection::WriteOnly : TrapProtection::ReadWrite};
        handle->value.protection = protection;
        ReprotectIntervals(handle->intervals, protection);
    }

    void TrapManager::RemoveTrap(TrapHandle handle) {
        TRACE_EVENT("host", "TrapManager::RemoveTrap");
        std::scoped_lock lock{trapMutex};
        handle->value.protection = TrapProtection::None;
        ReprotectIntervals(handle->intervals, TrapProtection::None);
    }

    void TrapManager::DeleteTrap(TrapHandle handle) {
        TRACE_EVENT("host", "TrapManager::DeleteTrap");
        std::scoped_lock lock{trapMutex};
        handle->value.protection = TrapProtection::None;
        ReprotectIntervals(handle->intervals, TrapProtection::None);
        trapMap.Remove(handle);
    }

    void TrapManager::ReprotectIntervals(const std::vector<TrapMap::Interval> &intervals, TrapProtection protection) {
        TRACE_EVENT("host", "TrapManager::ReprotectIntervals");
        for (auto region : intervals) {
            region = region.Align(constant::PageSize);
            if (protection == TrapProtection::ReadWrite) {
                mprotect(region.start, region.Size(), PROT_NONE);
                continue; // This trap covers the whole region at the strongest protection.
            }

            // A neighbour can share only the last page of a multi-page trap.
            // Applying that neighbour's protection to the whole old interval
            // strands its exclusive pages after DeleteTrap removes the callback.
            // Sweep page-aligned boundaries, not individual pages, and coalesce
            // equal permissions so isolated intervals still use one mprotect.
            struct Event {
                u8 *position;
                TrapProtection protection;
                bool starts;
            };
            std::vector<Event> events;
            trapMap.ForEachOverlappingInterval(region, [&](auto interval, const auto &entry) {
                if (entry.protection == TrapProtection::None)
                    return;
                interval = interval.Align(constant::PageSize);
                auto first{std::max(region.start, interval.start)};
                auto last{std::min(region.end, interval.end)};
                if (first < last) {
                    events.push_back({first, entry.protection, true});
                    events.push_back({last, entry.protection, false});
                }
            });
            std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
                return a.position < b.position;
            });

            std::array<size_t, 3> active{};
            auto runStart{region.start};
            int permissions{PROT_READ | PROT_WRITE | PROT_EXEC};
            for (auto event{events.begin()}; event != events.end();) {
                auto position{event->position};
                do {
                    auto &count{active[static_cast<size_t>(event->protection)]};
                    if (event->starts) ++count;
                    else --count;
                } while (++event != events.end() && event->position == position);

                const int next{active[static_cast<size_t>(TrapProtection::ReadWrite)] ? PROT_NONE :
                    active[static_cast<size_t>(TrapProtection::WriteOnly)] ? PROT_READ | PROT_EXEC :
                    PROT_READ | PROT_WRITE | PROT_EXEC};
                if (permissions != next) {
                    if (runStart < position)
                        mprotect(runStart, static_cast<size_t>(position - runStart), permissions);
                    runStart = position;
                    permissions = next;
                }
            }
            if (runStart < region.end)
                mprotect(runStart, static_cast<size_t>(region.end - runStart), permissions);
        }
    }

    static std::atomic<TrapManager *> staticTrap{nullptr};

    void TrapManager::InstallStaticInstance() {
        staticTrap.store(this, std::memory_order_seq_cst);
    }

    void TrapManager::UninstallStaticInstance() {
        // Only clear it if it's still our instance - avoids clobbering a newer instance that
        // may have already been installed by another KProcess in the meantime
        TrapManager *expected{this};
        staticTrap.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);
    }

    // A protection fault may already be pending when the final trap is deleted.
    // Check the kernel's current permissions without allocating or changing them.
    // Require RWX, exactly the permissions DeleteTrap restores for an untrapped
    // guest page: genuine unmapped, read-only and non-executable faults still fail.
    static bool IsUnprotectedGuestPage(u8 *address) {
        const int savedErrno{errno};
        const int fd{open("/proc/self/maps", O_RDONLY | O_CLOEXEC)};
        if (fd < 0) { errno = savedErrno; return false; }
        const auto target{reinterpret_cast<uintptr_t>(address)};
        char buffer[4096], prefix[64];
        size_t length{};
        bool accessible{}, done{};
        while (!done) {
            const auto count{read(fd, buffer, sizeof(buffer))};
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            for (ssize_t i{}; i < count; ++i) {
                if (buffer[i] != '\n') {
                    if (length < sizeof(prefix)) prefix[length++] = buffer[i];
                    continue;
                }
                uintptr_t first{}, last{};
                const auto begin{std::from_chars(prefix, prefix + length, first, 16)};
                if (begin.ec == std::errc{} && begin.ptr < prefix + length && *begin.ptr == '-') {
                    const auto end{std::from_chars(begin.ptr + 1, prefix + length, last, 16)};
                    if (end.ec == std::errc{} && prefix + length - end.ptr >= 4 && *end.ptr == ' ') {
                        if (first <= target && target < last) {
                            accessible = end.ptr[1] == 'r' && end.ptr[2] == 'w' && end.ptr[3] == 'x';
                            done = true;
                        } else if (first > target) done = true;
                    }
                }
                length = 0;
                if (done) break;
            }
        }
        close(fd);
        errno = savedErrno;
        return accessible;
    }

    bool TrapManager::TrapHandler(u8 *address, bool write, bool protectionFault) {
        if (!address) return false;
        auto *trap{staticTrap.load(std::memory_order_seq_cst)};
        if (trap == nullptr) [[unlikely]]
            return false; // No instance is currently installed, let the fault propagate normally rather than dereferencing a stale/null pointer
        if (trap->HandleTrap(address, write))
            return true;
        // Retry only an actual protection fault which has become accessible.
        // Do not turn an unrelated SIGSEGV into a successful trap or relax a page.
        return protectionFault && IsUnprotectedGuestPage(address);
    }

    bool TrapManager::HandleTrap(u8 *address, bool write) {
        TRACE_EVENT("host", "TrapManager::TrapHandler");

        LockCallback lockCallback{};
        // Keep owners through retries too: a blocking callback must not lose its
        // resource (and delete its trap) before the fault has been resolved.
        std::vector<std::shared_ptr<void>> callbackOwners;
        while (true) {
            if (lockCallback) {
                // We want to avoid a deadlock of holding trapMutex while locking the resource inside a callback while another thread holding the resource's mutex waits on trapMutex, we solve this by quitting the loop if a callback would be blocking and attempt to lock the resource externally
                lockCallback();
                lockCallback = {};
            }

            // The outer owner list is destroyed only after this lock is released.
            std::scoped_lock lock(trapMutex);

            // Retrieve any callbacks for the page that was faulted
            auto [entries, intervals]{trapMap.GetAlignedRecursiveRange<constant::PageSize>(address)};
            if (entries.empty())
                return false; // There's no callbacks associated with this page

            callbackOwners.reserve(callbackOwners.size() + entries.size());
            for (const auto &entry : entries) {
                auto owner{entry.get().owner.lock()};
                if (owner && std::find(callbackOwners.begin(), callbackOwners.end(), owner) == callbackOwners.end())
                    callbackOwners.push_back(std::move(owner));
            }

            // Do callbacks for every entry in the intervals
            if (write) {
                for (auto entryRef : entries) {
                    auto &entry{entryRef.get()};
                    if (entry.protection == TrapProtection::None)
                        // We don't need to do the callback if the entry doesn't require any protection already
                        continue;

                    if (!entry.writeCallback()) {
                        lockCallback = entry.lockCallback;
                        break;
                    }
                    entry.protection = TrapProtection::None; // We don't need to protect this entry anymore
                }
                if (lockCallback)
                    continue; // We need to retry the loop because a callback was blocking
            } else {
                bool allNone{true}; // If all entries require no protection, we can protect to allow all accesses
                for (auto entryRef : entries) {
                    auto &entry{entryRef.get()};
                    if (entry.protection < TrapProtection::ReadWrite) {
                        // We don't need to do the callback if the entry can already handle read accesses
                        allNone = allNone && entry.protection == TrapProtection::None;
                        continue;
                    }

                    if (!entry.readCallback()) {
                        lockCallback = entry.lockCallback;
                        break;
                    }
                    entry.protection = TrapProtection::WriteOnly; // We only need to trap writes to this entry
                }
                if (lockCallback)
                    continue; // We need to retry the loop because a callback was blocking
                write = allNone;
            }

            int permission{PROT_READ | (write ? PROT_WRITE : 0) | PROT_EXEC};
            for (const auto &interval : intervals)
                // Reprotect the interval to the lowest protection level that the callbacks performed allow
                mprotect(interval.start, interval.Size(), permission);

            return true;
        }
    }
}
