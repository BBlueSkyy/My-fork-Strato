// SPDX-License-Identifier: MPL-2.0
// Diagnostic-only JIT interface. This parses plugin metadata, but never runs guest code.

#include <algorithm>
#include <array>
#include <cstring>
#include <elf.h>
#include <string_view>
#include <mbedtls/sha256.h>
#include <kernel/results.h>
#include <kernel/types/KTransferMemory.h>
#include <loader/nro.h>
#include "IJitService.h"

namespace skyline::service::jit {
    namespace {
        // nn::ro::detail::NrrHeader, see NRR0 on Switchbrew.
        constexpr size_t NrrHeaderSize{0x350};
        constexpr size_t NrrSizeOffset{0x338};
        constexpr size_t NrrKindOffset{0x33C};
        constexpr size_t NrrHashesOffset{0x340};
        constexpr size_t NrrHashesCount{0x344};
        constexpr size_t HashSize{0x20};

        bool ContainsBytes(span<u8> data, size_t offset, size_t size) {
            return offset <= data.size() && size <= data.size() - offset;
        }

        u32 ReadU32(span<u8> data, size_t offset) {
            u32 value{};
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }

        bool HasSymbol(span<u8> image, const loader::NroHeader &header, std::string_view name) {
            if (!ContainsBytes(image, header.dynsym.offset, header.dynsym.size) ||
                !ContainsBytes(image, header.dynstr.offset, header.dynstr.size) ||
                (header.dynsym.size % sizeof(Elf64_Sym)) != 0)
                return false;

            const auto strings{image.subspan(header.dynstr.offset, header.dynstr.size)};
            const size_t count{header.dynsym.size / sizeof(Elf64_Sym)};
            for (size_t i{}; i < count; i++) {
                Elf64_Sym symbol{};
                std::memcpy(&symbol, image.data() + header.dynsym.offset + i * sizeof(Elf64_Sym), sizeof(symbol));

                if (symbol.st_name >= strings.size())
                    continue;

                const auto *start{reinterpret_cast<const char *>(strings.data() + symbol.st_name)};
                const size_t capacity{strings.size() - symbol.st_name};
                const auto *end{static_cast<const char *>(std::memchr(start, 0, capacity))};
                if (end && std::string_view{start, static_cast<size_t>(end - start)} == name)
                    return true;
            }
            return false;
        }
    }

    IJitService::IJitService(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager) {}

    IJitEnvironment::IJitEnvironment(const DeviceState &state, ServiceManager &manager,
                                     std::shared_ptr<kernel::type::KProcess> process,
                                     std::shared_ptr<kernel::type::KCodeMemory> executableMemory,
                                     std::shared_ptr<kernel::type::KCodeMemory> readableMemory)
        : BaseService(state, manager), process(std::move(process)),
          executableMemory(std::move(executableMemory)), readableMemory(std::move(readableMemory)) {}

    Result IJitService::CreateJitEnvironment(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.cmdArgSz < sizeof(u64) * 2)
            return kernel::result::InvalidArgument;

        const u64 executableSize{request.Pop<u64>()};
        const u64 readableSize{request.Pop<u64>()};
        LOGW("JIT_DIAG: CreateJitEnvironment exec_size=0x{:X}, ro_size=0x{:X}, copied_handles={}, moved_handles={}",
             executableSize, readableSize, request.copyHandles.size(), request.moveHandles.size());

        if (request.copyHandles.size() != 3) {
            LOGW("JIT_DIAG: CreateJitEnvironment expected exactly 3 copied handles");
            return kernel::result::InvalidHandle;
        }

        std::shared_ptr<kernel::type::KProcess> process;
        std::shared_ptr<kernel::type::KCodeMemory> executableMemory;
        std::shared_ptr<kernel::type::KCodeMemory> readableMemory;

        // The process and both CodeMemory handles are distinct kernel objects.
        // Validate them before exposing a diagnostic session to the guest.
        try {
            process = state.process->GetHandle<kernel::type::KProcess>(request.copyHandles.at(0));
            if (executableSize)
                executableMemory = state.process->GetHandle<kernel::type::KCodeMemory>(request.copyHandles.at(1));
            if (readableSize)
                readableMemory = state.process->GetHandle<kernel::type::KCodeMemory>(request.copyHandles.at(2));
        } catch (const std::exception &e) {
            LOGW("JIT_DIAG: CreateJitEnvironment invalid handle: {}", e.what());
            return kernel::result::InvalidHandle;
        }

        if (process != state.process ||
            (executableSize && (!executableMemory || executableSize > executableMemory->GetSource().size())) ||
            (readableSize && (!readableMemory || readableSize > readableMemory->GetSource().size()))) {
            LOGW("JIT_DIAG: CreateJitEnvironment process or CodeMemory size mismatch");
            return kernel::result::InvalidArgument;
        }

        LOGW("JIT_DIAG: validated JIT process and CodeMemory handles (exec=0x{:X}, ro=0x{:X}); mapping not implemented",
             executableSize, readableSize);
        manager.RegisterService(SRVREG(IJitEnvironment, std::move(process), std::move(executableMemory), std::move(readableMemory)), session, response);
        return {};
    }

    Result IJitEnvironment::GenerateCode(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGW("JIT_DIAG: GenerateCode called (args=0x{:X}, input_buffers={}, output_buffers={}); compiler unavailable",
             request.cmdArgSz, request.inputBuf.size(), request.outputBuf.size());
        return kernel::result::NotImplemented;
    }

    Result IJitEnvironment::Control(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGW("JIT_DIAG: Control called (args=0x{:X}, input_buffers={}, output_buffers={}); compiler unavailable",
             request.cmdArgSz, request.inputBuf.size(), request.outputBuf.size());
        return kernel::result::NotImplemented;
    }

    Result IJitEnvironment::LoadPlugin(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        // Actual command layout: u64 TransferMemorySize, one copied TransferMemory
        // handle, two input buffers (NRR followed by NRO).
        if (request.cmdArgSz < sizeof(u64) || request.copyHandles.size() != 1 || request.inputBuf.size() != 2) {
            LOGW("JIT_DIAG: LoadPlugin malformed IPC (args=0x{:X}, handles={}, buffers={})",
                 request.cmdArgSz, request.copyHandles.size(), request.inputBuf.size());
            return kernel::result::InvalidArgument;
        }

        const u64 workSize{request.Pop<u64>()};
        auto nrr{request.inputBuf[0]};
        auto nro{request.inputBuf[1]};
        LOGW("JIT_DIAG: LoadPlugin work_size=0x{:X}, NRR_size=0x{:X}, NRO_size=0x{:X}, handle=0x{:X}",
             workSize, nrr.size(), nro.size(), request.copyHandles[0]);

        std::shared_ptr<kernel::type::KTransferMemory> transferMemory;
        try {
            transferMemory = state.process->GetHandle<kernel::type::KTransferMemory>(request.copyHandles[0]);
        } catch (const std::exception &e) {
            LOGW("JIT_DIAG: LoadPlugin invalid TransferMemory handle: {}", e.what());
            return kernel::result::InvalidHandle;
        }

        if (!workSize || !util::IsPageAligned(workSize) || workSize > transferMemory->host.size()) {
            LOGW("JIT_DIAG: LoadPlugin invalid work_size 0x{:X} (backing=0x{:X})",
                 workSize, transferMemory->host.size());
            return kernel::result::InvalidSize;
        }

        if (nrr.size() < NrrHeaderSize || ReadU32(nrr, 0) != util::MakeMagic<u32>("NRR0")) {
            LOGW("JIT_DIAG: LoadPlugin NRR0 header missing or invalid");
            return kernel::result::InvalidArgument;
        }

        const u32 nrrSize{ReadU32(nrr, NrrSizeOffset)};
        const u32 hashOffset{ReadU32(nrr, NrrHashesOffset)};
        const u32 hashCount{ReadU32(nrr, NrrHashesCount)};
        const u8 nrrKind{nrr[NrrKindOffset]};
        if (nrrSize < NrrHeaderSize || nrrSize > nrr.size() ||
            nrrKind != 1 || hashOffset < NrrHeaderSize ||
            hashOffset > nrrSize || hashCount > (nrrSize - hashOffset) / HashSize) {
            LOGW("JIT_DIAG: LoadPlugin invalid NRR layout size=0x{:X}, kind={}, hash_offset=0x{:X}, count={}",
                 nrrSize, nrrKind, hashOffset, hashCount);
            return kernel::result::InvalidArgument;
        }

        if (nro.size() < sizeof(loader::NroHeader)) {
            LOGW("JIT_DIAG: LoadPlugin NRO too small");
            return kernel::result::InvalidArgument;
        }

        loader::NroHeader header{};
        std::memcpy(&header, nro.data(), sizeof(header));
        if (header.magic != util::MakeMagic<u32>("NRO0") ||
            header.size < sizeof(header) || header.size > nro.size()) {
            LOGW("JIT_DIAG: LoadPlugin invalid NRO header magic=0x{:X}, declared_size=0x{:X}",
                 header.magic, header.size);
            return kernel::result::InvalidArgument;
        }

        auto module{nro.subspan(0, header.size)};
        const auto segmentValid{[&](const loader::NroSegmentHeader &segment) {
            return ContainsBytes(module, segment.offset, segment.size);
        }};
        if (!segmentValid(header.text) || !segmentValid(header.ro) || !segmentValid(header.data) ||
            header.text.offset != 0 ||
            static_cast<u64>(header.text.offset) + header.text.size != header.ro.offset ||
            static_cast<u64>(header.ro.offset) + header.ro.size != header.data.offset ||
            static_cast<u64>(header.data.offset) + header.data.size != header.size) {
            LOGW("JIT_DIAG: LoadPlugin invalid NRO segment layout");
            return kernel::result::InvalidArgument;
        }

        std::array<u8, HashSize> nroHash{};
        if (mbedtls_sha256_ret(module.data(), module.size(), nroHash.data(), 0) != 0)
            return kernel::result::InvalidState;

        bool hashListed{};
        for (size_t i{}; i < hashCount; i++) {
            if (!std::memcmp(nrr.data() + hashOffset + i * HashSize, nroHash.data(), HashSize)) {
                hashListed = true;
                break;
            }
        }
        LOGW("JIT_DIAG: LoadPlugin NRR kind={}, hash_entries={}, NRO hash listed={}, segments text=0x{:X}, ro=0x{:X}, data=0x{:X}, bss=0x{:X}",
             nrrKind, hashCount, hashListed, header.text.size, header.ro.size, header.data.size, header.bssSize);

        if (!hashListed)
            return kernel::result::InvalidArgument;

        // Probe the dynamic symbol table without executing or relocating the NRO.
        // Checking inclusion in an NRR is NOT cryptographic signature verification.
        LOGW("JIT_DIAG: Plugin exports: GetVersion={}, Configure={}, Control={}, GenerateCode={}, OnPrepared={}, ResolveBasicSymbols={}, SetupDiagnostics={}",
             HasSymbol(module, header, "nnjitpluginGetVersion"),
             HasSymbol(module, header, "nnjitpluginConfigure"),
             HasSymbol(module, header, "nnjitpluginControl"),
             HasSymbol(module, header, "nnjitpluginGenerateCode"),
             HasSymbol(module, header, "nnjitpluginOnPrepared"),
             HasSymbol(module, header, "nnjitpluginResolveBasicSymbols"),
             HasSymbol(module, header, "nnjitpluginSetupDiagnostics"));

        LOGW("JIT_DIAG: NRR/NRO metadata validated; loading, relocation and guest plugin execution NOT implemented");
        return kernel::result::NotImplemented;
    }

    Result IJitEnvironment::GetCodeAddress(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        LOGW("JIT_DIAG: GetCodeAddress called; user CodeMemory aliases have not been mapped");
        return kernel::result::NotImplemented;
    }
}
