// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)
// Atmosphere cheat parsing and VM behavior derived from yuzu (GPL-2.0-or-later).

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sys/mman.h>
#include <unordered_map>
#include <unordered_set>
#include <input.h>
#include <os.h>
#include <kernel/scheduler.h>
#include <kernel/types/KProcess.h>
#include <kernel/types/KThread.h>
#include "cheat_manager.h"

namespace skyline::cheats {
    namespace {
        std::mutex mainNsoMutex;
        std::unordered_map<const kernel::type::KProcess *, MainNsoInfo> mainNsoInfos;

        std::string ToLower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::optional<std::filesystem::path> FindDirectory(const std::filesystem::path &parent, std::string_view name) {
            std::error_code error;
            for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
                if (it->is_directory(error) && !error && ToLower(it->path().filename().string()) == name)
                    return it->path();
            }
            return std::nullopt;
        }

        std::string BuildIdString(const std::array<u8, 0x20> &buildId) {
            std::string result;
            result.reserve(16);
            for (size_t index{}; index < 8; index++)
                result += fmt::format("{:02X}", buildId[index]);
            return result;
        }

        std::string EntryName(const CheatEntry &entry) {
            const auto end{std::find(entry.definition.name.begin(), entry.definition.name.end(), '\0')};
            return std::string(entry.definition.name.begin(), end);
        }

        std::vector<CheatEntry> ParseCheatText(std::string_view text) {
            std::vector<CheatEntry> entries(1);
            std::optional<size_t> current;

            for (size_t index{}; index < text.size();) {
                if (std::isspace(static_cast<unsigned char>(text[index]))) {
                    index++;
                    continue;
                }

                const char currentChar{text[index]};
                if (currentChar == '{' || currentChar == '[') {
                    const char closing{currentChar == '{' ? '}' : ']'};
                    const auto end{text.find(closing, index + 1)};
                    if (end == std::string_view::npos || end == index + 1)
                        return {};

                    if (currentChar == '{') {
                        current = 0;
                        if (entries[0].definition.numOpcodes)
                            return {};
                        entries[0].master = true;
                    } else {
                        current = entries.size();
                        entries.emplace_back();
                    }

                    auto &entry{entries[*current]};
                    const auto name{text.substr(index + 1, end - index - 1)};
                    const auto copySize{std::min(name.size(), entry.definition.name.size() - 1)};
                    std::memcpy(entry.definition.name.data(), name.data(), copySize);
                    entry.definition.name[copySize] = '\0';
                    index = end + 1;
                    continue;
                }

                if (!std::isxdigit(static_cast<unsigned char>(currentChar)) || !current || index + 8 > text.size())
                    return {};

                const auto word{text.substr(index, 8)};
                if (!std::all_of(word.begin(), word.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
                    return {};

                auto &entry{entries[*current]};
                if (entry.definition.numOpcodes >= entry.definition.opcodes.size())
                    return {};

                u32 value{};
                const auto result{std::from_chars(word.data(), word.data() + word.size(), value, 16)};
                if (result.ec != std::errc{} || result.ptr != word.data() + word.size())
                    return {};
                entry.definition.opcodes[entry.definition.numOpcodes++] = value;
                index += 8;
            }

            for (size_t index{}; index < entries.size(); index++) {
                entries[index].enabled = entries[index].definition.numOpcodes != 0;
                entries[index].id = static_cast<u32>(index);
            }
            return entries;
        }

        std::unordered_set<std::string> ReadDisabled(const std::filesystem::path &path) {
            std::unordered_set<std::string> disabled;
            std::ifstream input(path);
            std::string line;
            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty())
                    disabled.emplace(std::move(line));
            }
            return disabled;
        }

        std::vector<std::filesystem::path> EnabledModDirectories(const DeviceState &state, u64 titleId) {
            const auto root{std::filesystem::path(state.os->publicAppFilesPath) / "switch" / "load" / fmt::format("{:016X}", titleId)};
            std::vector<std::filesystem::path> result;
            std::error_code error;
            if (!std::filesystem::is_directory(root, error) || error)
                return result;

            for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
                if (!it->is_directory(error) || error)
                    continue;
                const auto path{it->path()};
                if (path.filename().string().starts_with('.'))
                    continue;
                error.clear();
                if (!std::filesystem::exists(path / ".disabled", error) && !error)
                    result.emplace_back(path);
                error.clear();
            }
            std::sort(result.begin(), result.end());
            return result;
        }

        std::optional<MainNsoInfo> GetMainNsoInfo(const kernel::type::KProcess *process) {
            std::scoped_lock lock{mainNsoMutex};
            auto it{mainNsoInfos.find(process)};
            if (it == mainNsoInfos.end())
                return std::nullopt;
            return it->second;
        }

        bool AccessGuestMemory(const std::shared_ptr<kernel::type::KProcess> &process, u64 address, void *buffer, size_t size, bool write) {
            if (!size)
                return true;
            if (address > std::numeric_limits<u64>::max() - size)
                return false;

            auto *guest{reinterpret_cast<u8 *>(address)};
            if (!process->memory.AddressSpaceContains(span<u8>{guest, size}))
                return false;

            auto *bytes{reinterpret_cast<u8 *>(buffer)};
            size_t copied{};
            while (copied < size) {
                auto *current{guest + copied};
                auto chunk{process->memory.GetChunk(current)};
                if (!chunk || chunk->second.state == memory::states::Unmapped ||
                    (write && !chunk->second.state.forceReadWritableByDebugSyscalls))
                    return false;

                const size_t chunkRemaining{chunk->second.size - static_cast<size_t>(current - chunk->first)};
                const uintptr_t pageAddress{reinterpret_cast<uintptr_t>(current) & ~(static_cast<uintptr_t>(constant::PageSize) - 1)};
                auto *pageGuest{reinterpret_cast<u8 *>(pageAddress)};
                const size_t pageOffset{static_cast<size_t>(current - pageGuest)};
                const size_t amount{std::min({
                    size - copied,
                    chunkRemaining,
                    static_cast<size_t>(constant::PageSize) - pageOffset,
                })};

                auto hostPage{process->memory.GetHostSpan(span<u8>{pageGuest, constant::PageSize})};
                auto *hostCurrent{hostPage.data() + pageOffset};
                process->trap.HandleTrap(hostCurrent, write);

                void *mirror{mremap(hostPage.data(), 0, constant::PageSize, MREMAP_MAYMOVE)};
                if (mirror == MAP_FAILED)
                    return false;
                if (mprotect(mirror, constant::PageSize, PROT_READ | PROT_WRITE)) {
                    munmap(mirror, constant::PageSize);
                    return false;
                }

                auto *mirrorCurrent{reinterpret_cast<u8 *>(mirror) + pageOffset};
                if (write) {
                    std::memcpy(mirrorCurrent, bytes + copied, amount);
                    __builtin___clear_cache(reinterpret_cast<char *>(mirrorCurrent), reinterpret_cast<char *>(mirrorCurrent + amount));
                    __builtin___clear_cache(reinterpret_cast<char *>(hostCurrent), reinterpret_cast<char *>(hostCurrent + amount));
                } else {
                    std::memcpy(bytes + copied, mirrorCurrent, amount);
                }
                munmap(mirror, constant::PageSize);
                copied += amount;
            }
            return true;
        }
    }

    void RecordMainNso(const kernel::type::KProcess *process, const std::array<u64, 4> &buildId, u64 base, u64 size) {
        MainNsoInfo info{.base = base, .size = size};
        std::memcpy(info.buildId.data(), buildId.data(), info.buildId.size());
        std::scoped_lock lock{mainNsoMutex};
        mainNsoInfos[process] = info;
    }

    void ClearMainNso(const kernel::type::KProcess *process) {
        std::scoped_lock lock{mainNsoMutex};
        mainNsoInfos.erase(process);
    }

    CheatManager::CheatManager(const DeviceState &state, std::shared_ptr<kernel::type::KProcess> process)
        : state{state}, process{std::move(process)}, vm{*this} {
        const auto info{GetMainNsoInfo(this->process.get())};
        if (!info)
            return;

        metadata.titleId = this->process->npdm.aci0.programId;
        metadata.mainNso = {info->base, info->size};
        metadata.heap = {
            reinterpret_cast<u64>(this->process->memory.heap.guest.data()),
            this->process->memory.heap.guest.size(),
        };
        metadata.alias = {
            reinterpret_cast<u64>(this->process->memory.alias.guest.data()),
            this->process->memory.alias.guest.size(),
        };
        metadata.aslr = {
            reinterpret_cast<u64>(this->process->memory.code.guest.data()),
            this->process->memory.code.guest.size(),
        };
        metadata.buildId = info->buildId;

        entries = LoadCheats();
        if (entries.empty())
            return;
        if (!vm.LoadProgram(entries)) {
            LOGW("Cheats: enabled opcodes exceed the 0x{:X} VM limit", CheatVm::MaximumProgramOpcodes);
            return;
        }
        if (!vm.ProgramSize())
            return;

        LOGI("Cheats: starting VM for {:016X}, Build ID {} with {} opcodes", metadata.titleId, BuildIdString(metadata.buildId), vm.ProgramSize());
        worker = std::thread([this] { Run(); });
    }

    CheatManager::~CheatManager() {
        stopRequested.store(true, std::memory_order_relaxed);
        if (worker.joinable())
            worker.join();
        if (processPaused)
            ResumeProcess();
        ClearMainNso(process.get());
    }

    std::vector<CheatEntry> CheatManager::LoadCheats() {
        std::vector<CheatEntry> result;
        const std::string buildId{BuildIdString(metadata.buildId)};

        for (const auto &mod : EnabledModDirectories(state, metadata.titleId)) {
            const auto cheatsDirectory{FindDirectory(mod, "cheats")};
            if (!cheatsDirectory)
                continue;

            std::vector<std::filesystem::path> files;
            std::error_code error;
            for (std::filesystem::directory_iterator it(*cheatsDirectory, error), end; !error && it != end; it.increment(error)) {
                if (!it->is_regular_file(error) || error)
                    continue;
                const auto path{it->path()};
                if (ToLower(path.extension().string()) == ".txt" && ToLower(path.stem().string()) == ToLower(buildId))
                    files.emplace_back(path);
            }
            std::sort(files.begin(), files.end());

            for (const auto &file : files) {
                std::ifstream input(file, std::ios::binary);
                if (!input) {
                    LOGW("Cheats: failed to open '{}'", file.string());
                    continue;
                }
                const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                auto parsed{ParseCheatText(text)};
                if (parsed.empty()) {
                    LOGW("Cheats: invalid cheat file '{}'", file.string());
                    continue;
                }

                const auto disabled{ReadDisabled(file.parent_path() / (file.stem().string() + ".disabled"))};
                size_t enabledCount{};
                for (auto &entry : parsed) {
                    if (!entry.master && disabled.contains(EntryName(entry)))
                        entry.enabled = false;
                    if (entry.enabled)
                        enabledCount++;
                    entry.id = static_cast<u32>(result.size());
                    result.emplace_back(std::move(entry));
                }
                LOGI("Cheats: loaded '{}' from '{}' ({} enabled entries)", file.filename().string(), mod.filename().string(), enabledCount);
            }
        }
        return result;
    }

    void CheatManager::Run() {
        constexpr auto interval{std::chrono::nanoseconds{1000000000 / 12}};
        auto next{std::chrono::steady_clock::now()};
        while (!stopRequested.load(std::memory_order_relaxed)) {
            vm.Execute(metadata);
            next += interval;
            std::this_thread::sleep_until(next);
            if (next < std::chrono::steady_clock::now() - interval)
                next = std::chrono::steady_clock::now();
        }
    }

    bool CheatManager::ReadMemory(u64 address, void *data, size_t size) {
        std::memset(data, 0, size);
        return AccessGuestMemory(process, address, data, size, false);
    }

    bool CheatManager::WriteMemory(u64 address, const void *data, size_t size) {
        return AccessGuestMemory(process, address, const_cast<void *>(data), size, true);
    }

    u64 CheatManager::KeysDown() {
        if (!state.input || !state.input->hid)
            return 0;

        std::scoped_lock lock{state.input->npad.mutex};
        u64 keys{};
        for (const auto &section : state.input->hid->npad) {
            const auto &info{section.defaultController};
            const auto index{info.header.currentEntry % constant::HidEntryCount};
            const auto &entry{info.state.at(index)};
            if (entry.status.connected)
                keys |= entry.buttons.raw;
        }
        return keys;
    }

    void CheatManager::PauseProcess() {
        if (processPaused)
            return;

        pausedThreads.clear();
        for (KHandle handle{constant::BaseHandleIndex};; handle++) {
            std::shared_ptr<kernel::type::KThread> thread;
            try {
                thread = process->GetHandle<kernel::type::KThread>(handle);
            } catch (const std::out_of_range &) {
                break;
            } catch (const std::exception &) {
                continue;
            }

            if (!thread || thread->killed || thread->isPaused)
                continue;

            std::scoped_lock migrationLock{thread->coreMigrationMutex};
            if (thread->coreId < constant::CoreCount) {
                state.scheduler->PauseThread(thread);
            } else {
                thread->isPaused = true;
                thread->insertThreadOnResume = true;
            }
            pausedThreads.emplace_back(std::move(thread));
        }
        processPaused = true;
    }

    void CheatManager::ResumeProcess() {
        if (!processPaused)
            return;

        for (auto &thread : pausedThreads) {
            if (!thread || thread->killed)
                continue;
            std::scoped_lock migrationLock{thread->coreMigrationMutex};
            if (thread->coreId < constant::CoreCount) {
                state.scheduler->ResumeThread(thread);
            } else {
                thread->isPaused = false;
                thread->scheduleCondition.notify();
            }
        }
        pausedThreads.clear();
        processPaused = false;
    }

    void CheatManager::DebugLog(u8 id, u64 value) {
        LOGI("Cheats: DebugLog id={:X}, value={:016X}", id, value);
    }
}
