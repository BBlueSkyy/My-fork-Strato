// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "KSyncObject.h"
#include <limits>
#include "KThread.h"

namespace skyline::kernel::type {
    void KSyncObject::Signal() {
        std::scoped_lock lock{syncObjectMutex};
        signalled = true;

        if (state.thread) {
            state.thread->RecordDiagnosticActivity(DiagnosticActivityType::SyncSignal,
                                                   static_cast<u32>(objectType), 0,
                                                   reinterpret_cast<uintptr_t>(this),
                                                   syncObjectWaiters.size(),
                                                   "KSyncObject::Signal");
        }

        for (auto &waiter : syncObjectWaiters) {
            if (waiter->isCancellable) {
                waiter->isCancellable = false;
                waiter->wakeObject = this;
                waiter->RecordDiagnosticActivity(DiagnosticActivityType::SyncWake,
                                                 static_cast<u32>(objectType), 0,
                                                 reinterpret_cast<uintptr_t>(this),
                                                 state.thread ? state.thread->id : std::numeric_limits<u64>::max(),
                                                 "KSyncObject::Signal");
                state.scheduler->InsertThread(waiter);
            }
        }
    }

    bool KSyncObject::ResetSignal() {
        std::scoped_lock lock{syncObjectMutex};
        if (signalled) [[likely]] {
            signalled = false;
            return true;
        }
        return false;
    }
}
