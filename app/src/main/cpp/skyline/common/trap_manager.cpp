// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2024 Strato Team and Contributors (https://github.com/strato-emu/)

#include <sys/mman.h>
#include <algorithm>
#include <atomic>
#include <vector>
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
        handle->value.protection = writeOnly ? TrapProtection::WriteOnly : TrapProtection::ReadWrite;
        ReprotectIntervals(handle->intervals);
    }

    void TrapManager::RemoveTrap(TrapHandle handle) {
        TRACE_EVENT("host", "TrapManager::RemoveTrap");
        std::scoped_lock lock{trapMutex};
        handle->value.protection = TrapProtection::None;
        ReprotectIntervals(handle->intervals);
    }

    void TrapManager::DeleteTrap(TrapHandle handle) {
        TRACE_EVENT("host", "TrapManager::DeleteTrap");
        std::scoped_lock lock{trapMutex};
        // Remove the entry first so that the recomputation below only sees the traps that remain: the pages this handle covered must end up with exactly the
        // protection that the *remaining* topology requires, and a copy of the intervals outlives the handle
        const auto intervals{handle->intervals};
        trapMap.Remove(handle);
        ReprotectIntervals(intervals);
    }

    void TrapManager::ReprotectIntervals(const std::vector<TrapMap::Interval> &intervals) {
        TRACE_EVENT("host", "TrapManager::ReprotectIntervals");

        struct Boundary {
            u8 *address;
            int level; //!< TrapProtection of the interval that starts (+1) or ends (-1) here
            int delta;
        };

        for (auto region : intervals) {
            region = region.Align(constant::PageSize);
            if (region.start >= region.end)
                continue;

            // The protection of a page is decided by the strongest protection among the entries that overlap *that page*. Deciding it once for the whole
            // region from every entry that merely touches it leaves pages protected for traps that only cover a different part of the region, and once
            // those traps are gone nothing is left that could ever handle (or lift) the protection
            std::vector<Boundary> boundaries;
            trapMap.ForEachOverlap({region.start, region.end}, [&](const TrapMap::Interval &entryInterval, const CallbackEntry &entry) {
                if (entry.protection == TrapProtection::None)
                    return;
                const auto aligned{entryInterval.Align(constant::PageSize)};
                boundaries.push_back({std::max(aligned.start, region.start), static_cast<int>(entry.protection), +1});
                boundaries.push_back({std::min(aligned.end, region.end), static_cast<int>(entry.protection), -1});
            });
            std::sort(boundaries.begin(), boundaries.end(), [](const Boundary &a, const Boundary &b) { return a.address < b.address; });

            const auto protectionFor{[](const int (&active)[3]) {
                if (active[static_cast<int>(TrapProtection::ReadWrite)])
                    return PROT_NONE;
                if (active[static_cast<int>(TrapProtection::WriteOnly)])
                    return PROT_READ | PROT_EXEC;
                return PROT_READ | PROT_WRITE | PROT_EXEC;
            }};

            int active[3]{};
            u8 *segmentStart{region.start};
            int segmentProtection{protectionFor(active)};
            const auto flush{[&](u8 *end) {
                if (end > segmentStart)
                    mprotect(segmentStart, static_cast<size_t>(end - segmentStart), segmentProtection);
                segmentStart = end;
            }};

            for (size_t i{}; i < boundaries.size();) {
                u8 *address{boundaries[i].address};
                for (; i < boundaries.size() && boundaries[i].address == address; i++)
                    active[boundaries[i].level] += boundaries[i].delta;

                const int protection{protectionFor(active)};
                if (protection != segmentProtection) {
                    flush(address); // Adjacent pages with the same protection are applied together to avoid splitting the mapping needlessly
                    segmentProtection = protection;
                }
            }
            flush(region.end);
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
                return false; // There's no callbacks associated with this page

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
