// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "guest.h"

namespace skyline::kernel::type { class KSharedMemory; }

namespace skyline::nce::diagnostics {
    void BeginSvc(u16 svcId, const ThreadContext &ctx);
    void EndSvc(const DeviceState &state, const ThreadContext &ctx);
    void RecordIpc(const char *name, u32 result);
    void ArmAudioTrace();
    void DisarmAudioTrace();
    void WatchTimeSharedMemory(const std::shared_ptr<kernel::type::KSharedMemory> &memory);
    void DumpGuestContext(const DeviceState &state, const char *event);
    void DumpBreak(const DeviceState &state, u64 reason, u64 info, u64 size);
}
