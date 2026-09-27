// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include <common/trace.h>
#include <kernel/types/KProcess.h>
#include <kernel/svc.h>
#include <common/signal.h>
#include "jit_core_32.h"
#include "exception.h"

namespace skyline::jit {
    JitCore32::JitCore32(const DeviceState &state, Dynarmic::ExclusiveMonitor &monitor, u32 coreId)
        : state{state}, monitor{monitor}, coreId{coreId}, jit{MakeDynarmicJit()} {}

    Dynarmic::A32::Jit JitCore32::MakeDynarmicJit() {
        coproc15 = std::make_shared<Coprocessor15>();

        Dynarmic::A32::UserConfig config;

        config.callbacks = this;
        config.processor_id = coreId;
        config.global_monitor = &monitor;

        config.coprocessors[15] = coproc15;

        // Enable "safe" unsafe optimizations
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
        // Keep the global monitor enabled: 32-bit guest threads use exclusive accesses to synchronize.
        config.unsafe_optimizations = true;

        config.fastmem_pointer = reinterpret_cast<uintptr_t>(state.process->memory.base.data());
        config.fastmem_exclusive_access = true;

        config.define_unpredictable_behaviour = true;

        config.wall_clock_cntpct = true;
        config.enable_cycle_counting = false;

        return Dynarmic::A32::Jit{config};
    }

    void JitCore32::Run(ThreadContext32 &context, u32 tlsPointer) {
        HaltReason haltReason;
        u32 swi;
        {
            // The scheduler may wake a replacement before the previous host thread leaves
            // Dynarmic. Keep restoration, execution and saving under the same core lease.
            std::scoped_lock lock{runMutex};
            // A delayed signal for the previous owner can leave a halt bit set.
            ClearHalt(static_cast<HaltReason>(static_cast<u32>(HaltReason::Preempted) |
                static_cast<u32>(HaltReason::Fault)));
            // Publish ownership before testing the pending flag. A yield that arrives
            // afterward can halt this core even before Dynarmic enters Run().
            state.thread->jit = this;
            if (kernel::Scheduler::YieldPending || state.thread->killed) {
                state.thread->jit = nullptr;
                ClearHalt(static_cast<HaltReason>(static_cast<u32>(HaltReason::Preempted) |
                    static_cast<u32>(HaltReason::Fault)));
                return;
            }

            RestoreContext(context);
            SetThreadPointer(context.tpidr);
            SetTlsPointer(tlsPointer);

            try {
                haltReason = static_cast<HaltReason>(jit.Run());
            } catch (...) {
                state.thread->jit = nullptr;
                throw;
            }

            // Signals delivered during context saving must only set YieldPending;
            // they may no longer access this core after guest execution returns.
            state.thread->jit = nullptr;

            // SVCs can block or migrate the calling thread, so finish using the shared
            // JIT before dispatching them and keep their results in the thread context.
            SaveContext(context);
            ClearHalt(static_cast<HaltReason>(static_cast<u32>(haltReason) |
                static_cast<u32>(HaltReason::Preempted) | static_cast<u32>(HaltReason::Fault)));
            swi = lastSwi;
        }

        if (const int fault{state.thread->jitFaultSignal.exchange(0)}) {
            signal::SignalException error;
            error.signal = fault;
            error.pc = reinterpret_cast<void *>(state.thread->jitFaultPc);
            error.fault = reinterpret_cast<void *>(state.thread->jitFaultAddress);
            throw error;
        }

        if (state.thread->killed)
            return;

        // Dynarmic returns a bitmask: an SVC and a preemption may be requested
        // together. The SVC must still run so the guest receives its result.
        const bool preempted{HasHaltReason(haltReason, HaltReason::Preempted)};
        const bool hasSvc{HasHaltReason(haltReason, HaltReason::Svc)};
        if (preempted)
            state.thread->isPreempted = false;

        const u32 unexpectedReasons{static_cast<u32>(haltReason) &
            ~(static_cast<u32>(HaltReason::Svc) | static_cast<u32>(HaltReason::Preempted) | static_cast<u32>(HaltReason::Fault))};
        if (unexpectedReasons || (!hasSvc && !preempted))
            LOGE("JIT halted: {} (0x{:X})", to_string(haltReason), static_cast<u32>(haltReason));

        if (hasSvc)
            SvcHandler(swi, context);
    }

    void JitCore32::HaltExecution(HaltReason hr) {
        jit.HaltExecution(ToDynarmicHaltReason(hr));
    }

    void JitCore32::ClearHalt(HaltReason hr) {
        jit.ClearHalt(ToDynarmicHaltReason(hr));
    }

    void JitCore32::SaveContext(ThreadContext32 &context) {
        context.gpr = jit.Regs();
        context.fpr = jit.ExtRegs();
        context.cpsr = jit.Cpsr();
        context.fpscr = jit.Fpscr();
        context.tpidr = GetThreadPointer();
    }

    void JitCore32::RestoreContext(const ThreadContext32 &context) {
        jit.Regs() = context.gpr;
        jit.ExtRegs() = context.fpr;
        jit.SetCpsr(context.cpsr);
        jit.SetFpscr(context.fpscr);
    }

    void JitCore32::SetThreadPointer(u32 threadPtr) {
        coproc15->tpidrurw = threadPtr;
    }

    u32 JitCore32::GetThreadPointer() const {
        return coproc15->tpidrurw;
    }

    void JitCore32::SetTlsPointer(u32 tlsPtr) {
        coproc15->tpidruro = tlsPtr;
    }

    u32 JitCore32::GetPC() {
        return jit.Regs()[15];
    }

    void JitCore32::SetPC(u32 pc) {
        jit.Regs()[15] = pc;
    }

    u32 JitCore32::GetSP() {
        return jit.Regs()[13];
    }

    void JitCore32::SetSP(u32 sp) {
        jit.Regs()[13] = sp;
    }

    u32 JitCore32::GetRegister(u32 reg) {
        return jit.Regs()[reg];
    }

    void JitCore32::SetRegister(u32 reg, u32 value) {
        jit.Regs()[reg] = value;
    }

    void JitCore32::SvcHandler(u32 swi, ThreadContext32 &context) {
        auto svc{kernel::svc::SvcTable[swi]};
        if (svc) [[likely]] {
            TRACE_EVENT("kernel", perfetto::StaticString{svc.name});
            auto svcContext = jit::MakeSvcContext(context);
            (svc.function)(state, svcContext);
            jit::ApplySvcContext(svcContext, context);
        } else {
            throw exception("Unimplemented SVC 0x{:X}", swi);
        }
    }

    template<typename T>
    __attribute__((__always_inline__)) T ReadUnaligned(u8 *ptr) {
        T value;
        std::memcpy(&value, ptr, sizeof(T));
        return value;
    }

    template<typename T>
    __attribute__((__always_inline__)) void WriteUnaligned(u8 *ptr, T value) {
        std::memcpy(ptr, &value, sizeof(T));
    }

    template<typename T>
    __attribute__((__always_inline__)) T JitCore32::MemoryRead(u32 vaddr) {
        // The number of bits needed to encode the size of T minus 1
        constexpr u32 bits = std::bit_width(sizeof(T)) - 1;
        // Compute the mask to have "bits" number of 1s (e.g. 0b111 for 3 bits)
        constexpr u32 mask{(1 << bits) - 1};

        if ((vaddr & mask) == 0) // Aligned access
            return state.process->memory.base.cast<T>()[vaddr >> bits];
        else
            return ReadUnaligned<T>(state.process->memory.base.data() + vaddr);
    }

    template<typename T>
    __attribute__((__always_inline__)) void JitCore32::MemoryWrite(u32 vaddr, T value) {
        // The number of bits needed to encode the size of T minus 1
        constexpr u32 bits = std::bit_width(sizeof(T)) - 1;
        // Compute the mask to have "bits" number of 1s (e.g. 0b111 for 3 bits)
        constexpr u32 mask{(1 << bits) - 1};

        if ((vaddr & mask) == 0) // Aligned access
            state.process->memory.base.cast<T>()[vaddr >> bits] = value;
        else
            WriteUnaligned<T>(state.process->memory.base.data() + vaddr, value);
    }

    template<typename T>
    __attribute__((__always_inline__)) bool JitCore32::MemoryWriteExclusive(u32 vaddr, T value, T expected) {
        auto ptr = reinterpret_cast<T *>(state.process->memory.base.data() + vaddr);
        // Sync built-ins should handle unaligned accesses
        return __sync_bool_compare_and_swap(ptr, expected, value);
    }

    u8 JitCore32::MemoryRead8(u32 vaddr) {
        return MemoryRead<u8>(vaddr);
    }

    u16 JitCore32::MemoryRead16(u32 vaddr) {
        return MemoryRead<u16>(vaddr);
    }

    u32 JitCore32::MemoryRead32(u32 vaddr) {
        return MemoryRead<u32>(vaddr);
    }

    u64 JitCore32::MemoryRead64(u32 vaddr) {
        return MemoryRead<u64>(vaddr);
    }

    void JitCore32::MemoryWrite8(u32 vaddr, u8 value) {
        MemoryWrite<u8>(vaddr, value);
    }

    void JitCore32::MemoryWrite16(u32 vaddr, u16 value) {
        MemoryWrite<u16>(vaddr, value);
    }

    void JitCore32::MemoryWrite32(u32 vaddr, u32 value) {
        MemoryWrite<u32>(vaddr, value);
    }

    void JitCore32::MemoryWrite64(u32 vaddr, u64 value) {
        MemoryWrite<u64>(vaddr, value);
    }

    bool JitCore32::MemoryWriteExclusive8(u32 vaddr, std::uint8_t value, std::uint8_t expected) {
        return MemoryWriteExclusive<u8>(vaddr, value, expected);
    }

    bool JitCore32::MemoryWriteExclusive16(u32 vaddr, std::uint16_t value, std::uint16_t expected) {
        return MemoryWriteExclusive<u16>(vaddr, value, expected);
    }

    bool JitCore32::MemoryWriteExclusive32(u32 vaddr, std::uint32_t value, std::uint32_t expected) {
        return MemoryWriteExclusive<u32>(vaddr, value, expected);
    }

    bool JitCore32::MemoryWriteExclusive64(u32 vaddr, std::uint64_t value, std::uint64_t expected) {
        return MemoryWriteExclusive<u64>(vaddr, value, expected);
    }

    void JitCore32::InterpreterFallback(u32 pc, size_t numInstructions) {
        LOGE("Interpreter fallback at 0x{:X} for {} instructions is not supported", pc, numInstructions);
        state.process->Kill(false, true);
    }

    void JitCore32::CallSVC(u32 swi) {
        lastSwi = swi;
        HaltExecution(HaltReason::Svc);
    }

    void JitCore32::ExceptionRaised(u32 pc, Dynarmic::A32::Exception exception) {
        LOGE("Exception raised at 0x{:X}: {}", pc, to_string(exception));
        const u32 cpsr{jit.Cpsr()};
        LOGE("AArch32 exception context: CPSR=0x{:X}, LR=0x{:X}, SP=0x{:X}, R0=0x{:X}, R1=0x{:X}, R2=0x{:X}, R3=0x{:X}",
             cpsr, GetRegister(14), GetSP(), GetRegister(0), GetRegister(1), GetRegister(2), GetRegister(3));

        auto region{span<u8>{reinterpret_cast<u8 *>(static_cast<uintptr_t>(pc & ~1U)), sizeof(u32)}};
        if (state.process->memory.AddressSpaceContains(region) && state.process->memory.IsRangeMapped(region)) {
            if (auto chunk{state.process->memory.GetChunk(region.data())}) {
                LOGE("AArch32 exception memory: type=0x{:X}, permissions={}",
                     chunk->second.state.value, chunk->second.permission);
                if (chunk->second.permission.r)
                    LOGE("AArch32 exception instruction: 0x{:08X}", MemoryRead32(pc & ~1U));
            }
        }
        state.process->Kill(false, true);
    }
}
