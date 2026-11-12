// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2024 Strato Team and Contributors (https://github.com/strato-emu/)

#include <sys/mman.h>
#include <atomic>
#include "trace.h"
#include "trap_manager.h"

namespace skyline {
    CallbackEntry::CallbackEntry(TrapProtection protection, LockCallback lockCallback, TrapCallback readCallback, TrapCallback writeCallback) : protection{protection}, lockCallback{std::move(lockCallback)}, readCallback{std::move(readCallback)}, writeCallback{std::move(writeCallback)} {}

    constexpr TrapHandle::TrapHandle(const TrapMap::GroupHandle &handle) : TrapMap::GroupHandle(handle) {}

    TrapHandle TrapManager::CreateTrap(span<span<u8>> regions, const LockCallback &lockCallback, const TrapCallback &readCallback, const TrapCallback &writeCallback) {
        TRACE_EVENT("host", "TrapManager::CreateTrap");
        std::scoped_lock lock{trapMutex};
        TrapHandle handle{trapMap.Insert(regions, CallbackEntry{TrapProtection::None, lockCallback, readCallback, writeCallback})};
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
                continue;
            }

            // Recompute only at aligned trap boundaries, without allocation.
            // One trap or identical aliases take one overlap scan and mprotect;
            // a shorter prefix must not protect the removed extension's pages.
            auto *runStart{region.start};
            int runProtection{-1};
            for (auto *position{region.start}; position < region.end;) {
                auto *next{region.end};
                TrapProtection strongest{TrapProtection::None};
                trapMap.ForEachRange(TrapMap::Interval{position, region.end}, [&](const auto &entry, const auto &interval) {
                    const auto covered{interval.Align(constant::PageSize)};
                    if (covered.start <= position) {
                        strongest = std::max(strongest, entry.protection);
                        next = std::min(next, covered.end);
                    } else {
                        next = std::min(next, covered.start);
                    }
                });
                const auto permissions{strongest == TrapProtection::ReadWrite ? PROT_NONE :
                    strongest == TrapProtection::WriteOnly ? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE | PROT_EXEC};
                if (runProtection != permissions) {
                    if (position != runStart)
                        mprotect(runStart, static_cast<size_t>(position - runStart), runProtection);
                    runStart = position;
                    runProtection = permissions;
                }
                position = next;
            }
            mprotect(runStart, static_cast<size_t>(region.end - runStart), runProtection);
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

    bool TrapManager::TrapHandler(u8 *address, bool write) {
        auto *trap{staticTrap.load(std::memory_order_seq_cst)};
        if (trap == nullptr) [[unlikely]]
            return false; // No instance is currently installed, let the fault propagate normally rather than dereferencing a stale/null pointer
        return trap->HandleTrap(address, write);
    }

    bool TrapManager::HandleTrap(u8 *address, bool write) {
        TRACE_EVENT("host", "TrapManager::TrapHandler");

        LockCallback lockCallback{};
        bool observedTrap{};
        while (true) {
            if (lockCallback) {
                // We want to avoid a deadlock of holding trapMutex while locking the resource inside a callback while another thread holding the resource's mutex waits on trapMutex, we solve this by quitting the loop if a callback would be blocking and attempt to lock the resource externally
                lockCallback();
                lockCallback = {};
            }

            std::scoped_lock lock(trapMutex);

            // Retrieve any callbacks for the page that was faulted
            auto [entries, intervals]{trapMap.GetAlignedRecursiveRange<constant::PageSize>(address)};
            if (entries.empty())
                // A resource can remove its trap while lockCallback waits outside
                // trapMutex (e.g. rollback of a temporary texture mapping). Let
                // that already observed fault retry the now unprotected page.
                return observedTrap;
            observedTrap = true;

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
