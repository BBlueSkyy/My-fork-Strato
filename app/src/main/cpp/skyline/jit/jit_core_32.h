// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>
#include <dynarmic/interface/A32/a32.h>
#include <kernel/svc_context.h>
#include "coproc_15.h"
#include "thread_context32.h"
#include "halt_reason.h"

namespace skyline::jit {
    class JitCore32 final : public Dynarmic::A32::UserCallbacks {
      private:
        const DeviceState &state;
        Dynarmic::ExclusiveMonitor &monitor;
        u32 coreId;
        u32 lastSwi{};
        std::shared_ptr<Coprocessor15> coproc15;
        Dynarmic::A32::Jit jit;
        Dynarmic::A32::Jit MakeDynarmicJit();

      public:
        JitCore32(const DeviceState &state, Dynarmic::ExclusiveMonitor &monitor, u32 coreId);
        void Run();
        void HaltExecution(HaltReason hr);
        void ClearHalt(HaltReason hr);
        void SaveContext(ThreadContext32 &context);
        void RestoreContext(const ThreadContext32 &context);
        kernel::svc::SvcContext MakeSvcContext();
        void ApplySvcContext(const kernel::svc::SvcContext &context);
        void SetThreadPointer(u32 threadPtr);
        void SetTlsPointer(u32 tlsPtr);
        u32 GetPC();
        void SetPC(u32 pc);
        u32 GetSP();
        void SetSP(u32 sp);
        u32 GetRegister(u32 reg);
        void SetRegister(u32 reg, u32 value);
        void SvcHandler(u32 swi);

        u8 MemoryRead8(u32 vaddr) override;
        u16 MemoryRead16(u32 vaddr) override;
        u32 MemoryRead32(u32 vaddr) override;
        u64 MemoryRead64(u32 vaddr) override;
        void MemoryWrite8(u32 vaddr, u8 value) override;
        void MemoryWrite16(u32 vaddr, u16 value) override;
        void MemoryWrite32(u32 vaddr, u32 value) override;
        void MemoryWrite64(u32 vaddr, u64 value) override;
        bool MemoryWriteExclusive8(u32 vaddr, std::uint8_t value, std::uint8_t expected) override;
        bool MemoryWriteExclusive16(u32 vaddr, std::uint16_t value, std::uint16_t expected) override;
        bool MemoryWriteExclusive32(u32 vaddr, std::uint32_t value, std::uint32_t expected) override;
        bool MemoryWriteExclusive64(u32 vaddr, std::uint64_t value, std::uint64_t expected) override;
        void InterpreterFallback(u32 pc, size_t numInstructions) override;
        void CallSVC(u32 swi) override;
        void ExceptionRaised(u32 pc, Dynarmic::A32::Exception exception) override;
        void AddTicks(u64 ticks) override {}
        u64 GetTicksRemaining() override { return 0; }

      private:
        template<typename T> T MemoryRead(u32 vaddr);
        template<typename T> void MemoryWrite(u32 vaddr, T value);
        template<typename T> bool MemoryWriteExclusive(u32 vaddr, T value, T expected);
    };
}
