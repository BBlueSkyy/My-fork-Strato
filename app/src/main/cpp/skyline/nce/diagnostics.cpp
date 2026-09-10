// SPDX-License-Identifier: MPL-2.0
// Temporary guest-abort diagnostics; no guest result or register is modified.

#include <cerrno>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <kernel/types/KProcess.h>
#include <kernel/types/KThread.h>
#include <kernel/types/KSharedMemory.h>
#include <kernel/svc.h>
#include <loader/loader.h>
#include "diagnostics.h"
#include "diagnostic_instructions.h"

namespace skyline::nce::diagnostics {
    namespace {
        struct Call {
            u64 sequence{}, pc{}, lr{};
            u16 id{};
            std::array<u64, 6> input{};
            std::array<u64, 2> output{};
        };
        struct IpcCall {
            u64 sequence{};
            const char *name{}; // ServiceFunctionDescriptor names are static strings.
            u32 result{};
        };
        thread_local struct Trace {
            u64 sequence{}, pc{}, sp{};
            u32 nzcv{};
            std::array<u64, 31> registers{};
            Call current{};
            std::array<Call, 32> calls{};
            std::array<Call, 32> nonQueryCalls{};
            std::array<IpcCall, 16> ipc{};
            std::array<IpcCall, 16> clockIpc{};
            std::array<IpcCall, 8> failedIpc{};
            size_t callCount{}, nonQueryCount{}, ipcCount{}, clockIpcCount{}, failedIpcCount{};
            std::weak_ptr<kernel::type::KSharedMemory> timeSharedMemory;
            bool audioTrace{};
            bool audioCodeDumped{}, clockCodeDumped{};
        } trace;

        struct PreserveErrno {
            int value{errno};
            ~PreserveErrno() { errno = value; }
        };

        void Write(std::string message) {
            // Abort logs must not be lost behind the asynchronous DEBUG queue.
            AsyncLogger::LogSync(AsyncLogger::LogLevel::Info, std::move(message), "GuestDiagnostic");
        }

        template<typename Function>
        void Diagnostic(const char *stage, Function &&function) {
            PreserveErrno preserve;
            if (!AsyncLogger::CheckLogLevel(AsyncLogger::LogLevel::Info))
                return;
            try {
                function();
            } catch (const signal::SignalException &error) {
                Write(fmt::format("Diagnostic stage \"{}\" failed: {}; continuing remaining capture", stage, error.what()));
            } catch (const std::exception &error) {
                Write(fmt::format("Diagnostic stage \"{}\" unavailable: {}; continuing remaining capture", stage, error.what()));
            }
        }

        // Never dereference arbitrary guest diagnostic pointers. Checking the VMM
        // alone is insufficient: another thread can unmap/protect a page afterwards.
        // Both kernel-assisted readers return errors/short reads instead of taking
        // a host SIGSEGV. Some Android/sandbox kernels disallow process_vm_readv;
        // /proc/self/mem is an optional reader of this same process only.
        size_t Read(const DeviceState &state, u64 address, void *destination, size_t size) {
            if (!size || address > UINT64_MAX - size ||
                !state.process->memory.AddressSpaceContains(span<u8>{reinterpret_cast<u8 *>(address), size}))
                return 0;
            size_t done{};
            while (done < size) {
                const auto ptr{reinterpret_cast<u8 *>(address + done)};
                const auto chunk{state.process->memory.GetChunk(ptr)};
                if (!chunk || !chunk->second.permission.r)
                    break;
                const auto offset{static_cast<size_t>(ptr - chunk->first)};
                if (offset >= chunk->second.size)
                    break;
                const auto count{std::min(size - done, chunk->second.size - offset)};
                const auto host{state.process->memory.GetHostSpan(span<u8>{ptr, count})};
                iovec local{static_cast<u8 *>(destination) + done, count};
                iovec remote{host.data(), count};
                auto copied{process_vm_readv(getpid(), &local, 1, &remote, 1, 0)};
                if (copied < 0 && (errno == EPERM || errno == EACCES || errno == ENOSYS)) {
                    const int fd{open("/proc/self/mem", O_RDONLY | O_CLOEXEC)};
                    if (fd >= 0) {
                        copied = pread(fd, local.iov_base, count, static_cast<off_t>(reinterpret_cast<uintptr_t>(host.data())));
                        close(fd);
                    }
                }
                if (copied <= 0)
                    break;
                done += static_cast<size_t>(copied);
                if (static_cast<size_t>(copied) != count)
                    break;
            }
            return done;
        }

        std::string Bytes(const u8 *data, size_t size) {
            std::string out;
            for (size_t i{}; i < size; i++)
                out += fmt::format("{:02X}{}", data[i], i + 1 == size ? "" : " ");
            out += " |";
            for (size_t i{}; i < size; i++)
                out += data[i] >= 0x20 && data[i] <= 0x7E ? static_cast<char>(data[i]) : '.';
            return out + '|';
        }

        void DumpBytes(const DeviceState &state, const std::string &label, u64 address, size_t size) {
            std::array<u8, 256> data{};
            const size_t requested{std::min(size, data.size())};
            const size_t copied{Read(state, address, data.data(), requested)};
            Write(fmt::format("{} address=0x{:X}, read=0x{:X}/0x{:X}{}", label, address, copied, requested,
                              copied != requested ? " (unreadable/truncated)" : ""));
            for (size_t i{}; i < copied; i += 32)
                Write(fmt::format("  0x{:X}: {}", address + i, Bytes(data.data() + i, std::min<size_t>(32, copied - i))));
        }

        void DumpRegion(const DeviceState &state, const char *label, u64 address) {
            const auto chunk{state.process->memory.GetChunk(reinterpret_cast<u8 *>(address))};
            if (!chunk) {
                Write(fmt::format("{} address=0x{:X}: outside guest address space", label, address));
                return;
            }
            Write(fmt::format("{} address=0x{:X}: base={}, size=0x{:X}, state=0x{:X}, type=0x{:X}, permission=0x{:X}, attributes=0x{:X}",
                              label, address, fmt::ptr(chunk->first), chunk->second.size, chunk->second.state.value,
                              static_cast<u32>(chunk->second.state.type), chunk->second.permission.raw, chunk->second.attributes.value));
        }

        void DumpHistory() {
            const size_t first{trace.callCount > trace.calls.size() ? trace.callCount - trace.calls.size() : 0};
            Write("Recent completed SVCs on this guest thread (raw outputs; nonzero is not necessarily an error):");
            for (size_t i{first}; i < trace.callCount; i++) {
                const auto &call{trace.calls[i % trace.calls.size()]};
                Write(fmt::format("seq={} svc=0x{:X} pc=0x{:X} lr=0x{:X} in=[{:X},{:X},{:X},{:X},{:X},{:X}] out=[{:X},{:X}]",
                                  call.sequence, call.id, call.pc, call.lr, call.input[0], call.input[1], call.input[2],
                                  call.input[3], call.input[4], call.input[5], call.output[0], call.output[1]));
            }
            const size_t ipcFirst{trace.ipcCount > trace.ipc.size() ? trace.ipcCount - trace.ipc.size() : 0};
            Write("Recent completed IPCs on this guest thread (raw service results):");
            for (size_t i{ipcFirst}; i < trace.ipcCount; i++) {
                const auto &call{trace.ipc[i % trace.ipc.size()]};
                Write(fmt::format("seq={} {} result=0x{:X} module={} description={}", call.sequence, call.name,
                                  call.result, call.result & 0x1FF, (call.result >> 9) & 0x1FFF));
            }
            Write("Recent completed SVCs excluding QueryMemory (retain calls preceding exception unwinding):");
            const size_t nonQueryFirst{trace.nonQueryCount > trace.nonQueryCalls.size() ? trace.nonQueryCount - trace.nonQueryCalls.size() : 0};
            for (size_t i{nonQueryFirst}; i < trace.nonQueryCount; i++) {
                const auto &call{trace.nonQueryCalls[i % trace.nonQueryCalls.size()]};
                Write(fmt::format("seq={} svc=0x{:X} pc=0x{:X} lr=0x{:X} in=[{:X},{:X},{:X},{:X},{:X},{:X}] out=[{:X},{:X}]",
                                  call.sequence, call.id, call.pc, call.lr, call.input[0], call.input[1], call.input[2],
                                  call.input[3], call.input[4], call.input[5], call.output[0], call.output[1]));
            }
            const auto dumpIpc{[](const auto &calls, size_t count, const char *label) {
                Write(label);
                const size_t first{count > calls.size() ? count - calls.size() : 0};
                for (size_t i{first}; i < count; i++) {
                    const auto &call{calls[i % calls.size()]};
                    Write(fmt::format("seq={} {} result=0x{:X} module={} description={}", call.sequence, call.name,
                                      call.result, call.result & 0x1FF, (call.result >> 9) & 0x1FFF));
                }
            }};
            dumpIpc(trace.clockIpc, trace.clockIpcCount, "Recent time/clock IPC results on this thread:");
            dumpIpc(trace.failedIpc, trace.failedIpcCount, "Recent nonzero IPC results (may include expected/optional failures):");
        }

        void DumpTimeSharedMemory() {
            if (auto memory{trace.timeSharedMemory.lock()}) {
                // The strong reference keeps this owned host mapping alive. No
                // untrusted guest pointer is dereferenced here.
                Write(fmt::format("Time shared memory: guest={}, host size=0x{:X}, host CNTFRQ={}, tick ns={}",
                                  fmt::ptr(memory->guest.data()), memory->host.size(), util::ClockFrequency, util::GetTimeNs()));
                for (size_t offset{}; offset < std::min<size_t>(memory->host.size(), 0x200); offset += 32)
                    Write(fmt::format("Time shmem +0x{:03X}: {}", offset, Bytes(memory->host.data() + offset, std::min<size_t>(32, memory->host.size() - offset))));
            }
        }

        void DumpContext(const DeviceState &state, const char *event) {
            Write(fmt::format("{} seq={} tid={} svc=0x{:X} PC=0x{:X} LR=0x{:X} SP=0x{:X} FP=0x{:X} NZCV=0x{:X}",
                              event, trace.current.sequence, state.thread->id, trace.current.id, trace.pc,
                              trace.registers[30], trace.sp, trace.registers[29], trace.nzcv));
            for (size_t i{}; i < trace.registers.size(); i++)
                Write(fmt::format("X{}=0x{:016X}", i, trace.registers[i]));

            DumpHistory();
            Diagnostic("stack bytes", [&] { DumpBytes(state, "guest stack bytes", trace.sp, 0x100); });
            std::vector<void *> frames{reinterpret_cast<void *>(trace.pc), reinterpret_cast<void *>(trace.registers[30])};
            Diagnostic("frame walk", [&] {
                u64 fp{trace.registers[29]};
                for (size_t i{}; i < 24; i++) {
                    std::array<u64, 2> frame{};
                    if ((fp & 0xF) || fp < trace.sp || fp - trace.sp > 0x100000 ||
                        Read(state, fp, frame.data(), sizeof(frame)) != sizeof(frame))
                        break;
                    frames.push_back(reinterpret_cast<void *>(frame[1]));
                    if (frame[0] <= fp)
                        break;
                    fp = frame[0];
                }
            });
            // Always print raw addresses before consulting symbolic bookkeeping.
            for (size_t i{}; i < frames.size(); i++)
                Write(fmt::format("Guest frame[{}] PC/LR=0x{:X}", i, reinterpret_cast<u64>(frames[i])));
            Diagnostic("stack symbols", [&] {
                auto callSites{frames};
                // A saved LR is after BL/BLR and may equal the next function's
                // start (observed at system_clock::now -> terminate). Keep the
                // real PC untouched and resolve call sites at LR - 4.
                for (size_t i{1}; i < callSites.size(); i++)
                    if (reinterpret_cast<u64>(callSites[i]) >= 4)
                        callSites[i] = reinterpret_cast<void *>(reinterpret_cast<u64>(callSites[i]) - 4);
                Write(fmt::format("Guest stack (frame[0] PC; later frames resolved at LR-4; raw LRs above):{}", state.loader->GetStackTrace(callSites)));
            });
            for (size_t i{}; i < std::min<size_t>(frames.size(), 8); i++) {
                const auto address{reinterpret_cast<u64>(frames[i])};
                if (address >= 0x60)
                    Diagnostic("frame instructions", [&] {
                        DumpBytes(state, fmt::format("frame[{}] AArch64 instructions (includes NCE patches)", i), address - 0x60, 0xA0);
                    });
            }
        }

        // Uninterpreted candidate objects, not assumed exception layouts. Following
        // two pointer levels can expose an RTTI name/message without calling any
        // guest vtable or assuming libc++'s private __cxa_exception ABI.
        void DumpCandidates(const DeviceState &state) {
            std::vector<std::pair<u64, unsigned>> pending;
            std::vector<u64> seen;
            // Prioritize callee-saved candidates: the C++ throw path commonly
            // keeps its arguments here while X0-X2 are reused for svcBreak.
            for (size_t i{19}; i < 29; i++)
                pending.emplace_back(trace.registers[i], 0);
            for (auto address : trace.registers)
                pending.emplace_back(address, 0);
            for (size_t i{}; i < pending.size() && seen.size() < 48; i++) {
                const auto [address, depth]{pending[i]};
                if (!address || std::find(seen.begin(), seen.end(), address) != seen.end())
                    continue;
                const auto chunk{state.process->memory.GetChunk(reinterpret_cast<u8 *>(address))};
                if (!chunk || !chunk->second.permission.r || chunk->second.permission.x)
                    continue;
                seen.push_back(address);
                std::array<u8, 128> data{};
                const size_t copied{Read(state, address, data.data(), data.size())};
                if (!copied)
                    continue;
                Write(fmt::format("Uninterpreted pointer candidate 0x{:X} depth={} bytes=0x{:X}", address, depth, copied));
                for (size_t offset{}; offset < copied; offset += 32)
                    Write(fmt::format("  0x{:X}: {}", address + offset, Bytes(data.data() + offset, std::min<size_t>(32, copied - offset))));
                if (depth < 2) {
                    for (size_t offset{}; offset + sizeof(u64) <= std::min<size_t>(copied, 64); offset += sizeof(u64)) {
                        u64 pointer{};
                        std::memcpy(&pointer, data.data() + offset, sizeof(pointer));
                        pending.emplace_back(pointer, depth + 1);
                    }
                }
            }
        }

    }

    // Capture relevant bodies, rather than only a window around a return
    // address. Static references do not imply that a branch was executed.
    static void DumpFunctions(const DeviceState &state, std::initializer_list<std::string_view> roots) {
        struct Pending { u64 address; unsigned depth; };
        std::vector<Pending> pending;
        for (auto fragment : roots)
            for (const auto &symbol : state.loader->FindFunctionSymbols64(fragment, 12))
                pending.push_back({symbol.address, 0});
        Write(fmt::format("Function snapshots: {} roots, maximum 48 bodies / 0x10000 code bytes / 64 data references", pending.size()));
        std::vector<u64> seen, dataSeen;
        size_t remaining{0x10000}, item{};
        for (; item < pending.size() && seen.size() < 48 && remaining; item++) {
            const auto [target, depth]{pending[item]};
            auto symbol{state.loader->ResolveSymbol64(reinterpret_cast<void *>(target))};
            if (symbol.executableName.empty() || symbol.executableName.ends_with(".patch") || symbol.executableName.ends_with(".hook"))
                continue;
            const u64 start{symbol.address ? symbol.address : target};
            if (std::find(seen.begin(), seen.end(), start) != seen.end())
                continue;
            const auto chunk{state.process->memory.GetChunk(reinterpret_cast<u8 *>(start))};
            if (!chunk || !chunk->second.permission.x)
                continue;
            seen.push_back(start);
            std::array<u8, 0x1000> data{};
            const size_t requested{std::min({symbol.size ? symbol.size : size_t{0x200}, data.size(), remaining})};
            const size_t copied{Read(state, start, data.data(), requested)};
            remaining -= requested;
            Write(fmt::format("Function snapshot address=0x{:X} name={} module={} depth={} symbolSize=0x{:X} read=0x{:X}/0x{:X} (unknown symbols use a bounded window)",
                              start, symbol.name ? symbol.name : "<unknown>", symbol.executableName, depth, symbol.size, copied, requested));
            for (size_t offset{}; offset < copied; offset += 32)
                Write(fmt::format("  0x{:X}: {}", start + offset, Bytes(data.data() + offset, std::min<size_t>(32, copied - offset))));
            for (size_t offset{}; offset + 4 <= copied; offset += 4) {
                u32 word{}, next{};
                std::memcpy(&word, data.data() + offset, 4);
                if (offset + 8 <= copied)
                    std::memcpy(&next, data.data() + offset + 4, 4);
                if (auto branch{instructions::DirectBranch(word, start + offset)};
                    branch && (*branch < start || *branch - start >= (symbol.size ? symbol.size : copied))) {
                    u64 destination{*branch};
                    // Resolve standard AArch64 PLT stubs through their GOT
                    // slots after relocation, without executing guest code.
                    std::array<u32, 4> stub{};
                    if (Read(state, destination, stub.data(), sizeof(stub)) == sizeof(stub)) {
                        auto slot{instructions::DataReference(stub[0], stub[1], destination)};
                        const auto branchReg{(stub[3] >> 5) & 31};
                        const auto baseReg{stub[0] & 31};
                        if (slot && slot->pointerLoad && (stub[3] & 0xFFFFFC1F) == 0xD61F0000 &&
                            branchReg == (stub[1] & 31) && branchReg != baseReg &&
                            (stub[2] & 0xFFC00000) == 0x91000000 &&
                            (stub[2] & 31) == baseReg && ((stub[2] >> 5) & 31) == baseReg) {
                            u64 resolved{};
                            if (Read(state, slot->address, &resolved, sizeof(resolved)) == sizeof(resolved) && resolved)
                                destination = resolved;
                        }
                    }
                    const auto callee{state.loader->ResolveSymbol64(reinterpret_cast<void *>(destination))};
                    Write(fmt::format("Static branch pc=0x{:X} target=0x{:X} resolved=0x{:X} name={}", start + offset, *branch, destination, callee.name ? callee.name : "<unknown>"));
                    if (depth < 3 && pending.size() < 256)
                        pending.push_back({destination, depth + 1});
                }
                auto reference{instructions::DataReference(word, next, start + offset)};
                if (!reference || dataSeen.size() >= 64 || std::find(dataSeen.begin(), dataSeen.end(), reference->address) != dataSeen.end())
                    continue;
                const auto region{state.process->memory.GetChunk(reinterpret_cast<u8 *>(reference->address))};
                if (!region || !region->second.permission.r || region->second.permission.x)
                    continue;
                dataSeen.push_back(reference->address);
                DumpBytes(state, fmt::format("Static data reference from pc=0x{:X}", start + offset), reference->address, 0x80);
                if (reference->pointerLoad && dataSeen.size() < 64) {
                    u64 pointer{};
                    if (Read(state, reference->address, &pointer, sizeof(pointer)) == sizeof(pointer)) {
                        const auto pointee{state.process->memory.GetChunk(reinterpret_cast<u8 *>(pointer))};
                        if (pointee && pointee->second.permission.r && !pointee->second.permission.x &&
                            std::find(dataSeen.begin(), dataSeen.end(), pointer) == dataSeen.end()) {
                            dataSeen.push_back(pointer);
                            DumpBytes(state, "Static reference pointee (uninterpreted)", pointer, 0x100);
                        }
                    }
                }
            }
        }
        Write(fmt::format("Function snapshots finished: bodies={} dataReferences={} unvisitedCandidates={} remainingCodeBudget=0x{:X}", seen.size(), dataSeen.size(), pending.size() - item, remaining));
    }

    void BeginSvc(u16 svcId, const ThreadContext &ctx) {
        // The saved LR points to BL LoadCtx; the B back into .text is two
        // instructions later. Decode its signed imm26 to recover SVC PC exactly.
        // This is our live generated trampoline, never a guest-supplied pointer.
        const u64 branchAddress{ctx.callSite.trampolineLr + 8};
        u32 branch{};
        std::memcpy(&branch, reinterpret_cast<const void *>(branchAddress), sizeof(branch));
        const i64 words{static_cast<i64>(branch & 0x03FFFFFF) - ((branch & 0x02000000) ? 0x04000000 : 0)};
        trace.pc = (branch & 0xFC000000) == 0x14000000 ? branchAddress + words * 4 - 4 : 0;
        trace.sp = ctx.callSite.sp;
        trace.nzcv = ctx.nzcv;
        std::copy(ctx.gpr.regs.begin(), ctx.gpr.regs.end(), trace.registers.begin());
        std::copy(ctx.callSite.x19ToX30.begin(), ctx.callSite.x19ToX30.end(), trace.registers.begin() + 19);
        trace.current = {.sequence = ++trace.sequence, .pc = trace.pc, .lr = trace.registers[30], .id = svcId};
        std::copy_n(trace.registers.begin(), trace.current.input.size(), trace.current.input.begin());
        if (svcId == 0x15 && trace.audioTrace)
            Diagnostic("transfer owner before", [&] { DumpRegion(*ctx.state, "CreateTransferMemory owner BEFORE", trace.current.input[1]); });
    }

    void EndSvc(const DeviceState &state, const ThreadContext &ctx) {
        trace.current.output = {ctx.gpr.x0, ctx.gpr.x1};
        trace.calls[trace.callCount++ % trace.calls.size()] = trace.current;
        if (trace.current.id != 0x6)
            trace.nonQueryCalls[trace.nonQueryCount++ % trace.nonQueryCalls.size()] = trace.current;
        if (trace.audioTrace && trace.current.id == 0x15) {
            Diagnostic("transfer return", [&] {
                Write(fmt::format("CreateTransferMemory RETURN result=0x{:X} handle=0x{:X}", ctx.gpr.x0, ctx.gpr.x1));
                DumpRegion(state, "CreateTransferMemory owner AFTER", trace.current.input[1]);
                DumpContext(state, "audio transfer-memory boundary");
            });
            if (!trace.audioCodeDumped) {
                trace.audioCodeDumped = true;
                Diagnostic("audio client functions", [&] { DumpFunctions(state, {"OpenAudioRenderer"}); });
            }
        } else if (trace.audioTrace && trace.current.id == 0x6 && ctx.gpr.x0 == 0) {
            Diagnostic("query memory return", [&] { DumpBytes(state, "QueryMemory RETURN MemoryInfo", trace.current.input[0], 0x28); });
        }
    }

    void RecordIpc(const char *name, u32 result) {
        const IpcCall call{trace.current.sequence, name, result};
        trace.ipc[trace.ipcCount++ % trace.ipc.size()] = call;
        const std::string_view method{name};
        if (method.find("Clock") != method.npos || method.find("Time") != method.npos || method.find("GetSharedMemoryNativeHandle") != method.npos)
            trace.clockIpc[trace.clockIpcCount++ % trace.clockIpc.size()] = call;
        if (result)
            trace.failedIpc[trace.failedIpcCount++ % trace.failedIpc.size()] = call;
    }

    void WatchTimeSharedMemory(const std::shared_ptr<kernel::type::KSharedMemory> &memory) {
        trace.timeSharedMemory = memory;
        Diagnostic("time shared memory exported", [] { DumpTimeSharedMemory(); });
    }

    void ArmAudioTrace() { trace.audioTrace = true; }
    void DisarmAudioTrace() { trace.audioTrace = false; }

    void DumpGuestContext(const DeviceState &state, const char *event) {
        Diagnostic("guest context", [&] { DumpContext(state, event); });
        Diagnostic("pointer candidates", [&] { DumpCandidates(state); });
    }

    void DumpBreak(const DeviceState &state, u64 reason, u64 info, u64 size) {
        Diagnostic("break reason", [&] {
            const auto code{reason & ~(1ULL << 31)};
            Write(fmt::format("svcBreak reason=0x{:X} code={} notificationOnly={} info=0x{:X} size=0x{:X}{}",
                              reason, code, (reason & (1ULL << 31)) != 0, info, size,
                              code == 7 ? " (CppException notification; not the exception type)" : ""));
        });
        // Payload comes first: an unrelated failure in symbolisation must not
        // hide the Result passed to a fatal Break. Each phase is independent.
        Diagnostic("break payload", [&] {
            if (size) {
                DumpBytes(state, "Break info (bounded to 0x100 bytes)", info, std::min<u64>(size, 0x100));
                if (size == sizeof(u32)) {
                    u32 value{};
                    if (Read(state, info, &value, sizeof(value)) == sizeof(value))
                        Write(fmt::format("Break info u32=0x{:X}; if a Result: module={} description={}", value, value & 0x1FF, (value >> 9) & 0x1FFF));
                }
            }
        });
        Diagnostic("break context", [&] { DumpContext(state, "svcBreak entry"); });
        Diagnostic("break pointer candidates", [&] { DumpCandidates(state); });
        Diagnostic("time shared memory at break", [] { DumpTimeSharedMemory(); });
        if ((reason & ~(1ULL << 31)) == 7 && !trace.clockCodeDumped) {
            trace.clockCodeDumped = true;
            Diagnostic("clock client functions", [&] {
                DumpFunctions(state, {"clock_gettime", "StandardUserSystemClock", "StandardSteadyClock"});
            });
        }
    }
}
