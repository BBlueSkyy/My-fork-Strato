// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <services/settings/settings_store.h>
#include <services/settings/settings_items.h>
#include <services/settings/ipc_helpers.h>
using namespace skyline;
using namespace skyline::service;
using namespace skyline::service::settings;
int main() {
    char temporary[] = "/tmp/strato-settings-XXXXXX";
    auto directory = mkdtemp(temporary);
    assert(directory);
    HostOS os{directory};
    auto config = std::make_shared<Settings>();
    config->isInternetEnabled = true;
    DeviceState state{&os, config};
    {
        SettingsStore store(state);
        assert(*store.Get<u32>(23, 0) == 0);
        assert(!store.Set<u32>(23, 1));
        assert(!store.Set<u8>(73, 0));
        assert(*store.Get<u32>(23, 0) == 1);
        assert(!store.Get<u8>(23, 0)); // Stored type mismatch must not become success.
        std::vector<u8> oversized(0x10001);
        assert(store.Set(21, span<const u8>(oversized)));
        // Force a real write failure after opening a valid store. No in-memory success.
        auto saved = std::string(directory) + "-moved";
        std::filesystem::rename(directory, saved);
        assert(store.Set<u32>(23, 0));
        assert(*store.Get<u32>(23, 0) == 1);
        std::filesystem::rename(saved, directory);
    }
    {
        SettingsStore reopened(state);
        assert(*reopened.Get<u32>(23, 0) == 1);
        assert(!*config->isInternetEnabled);
        assert(reopened.internetAllowed); // Guest cannot raise the original host policy.
    }
    // Corrupt/truncated persisted state is preserved and explicitly rejected.
    std::ofstream(std::string(directory) + "/system-settings.bin", std::ios::binary | std::ios::trunc) << "bad";
    {
        SettingsStore corrupt(state);
        assert(!corrupt.Get<u32>(23, 0));
        assert(corrupt.Set<u32>(23, 1));
        assert(std::filesystem::file_size(std::string(directory) + "/system-settings.bin") == 3);
    }
    ipc::IpcRequest request;
    assert(WriteBuffer(request, u32{0x12345678}));
    std::array<u8, 6> guarded{0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    request.outputBuf.emplace_back(guarded.data() + 1, 3);
    assert(WriteBuffer(request, u32{0x12345678}));
    assert(std::all_of(guarded.begin(), guarded.end(), [](u8 v) { return v == 0xCC; }));
    request.outputBuf[0] = span<u8>(guarded.data() + 1, 4);
    assert(!WriteBuffer(request, u32{0x12345678}));
    assert(guarded.front() == 0xCC && guarded.back() == 0xCC);
    assert(guarded[1] == 0x78 && guarded[4] == 0x12);
    request.cmdArg = guarded.data() + 1;
    request.cmdArgSz = 3;
    assert(!ReadArgument<u32>(request));
    request.cmdArgSz = 4;
    assert(*ReadArgument<u32>(request) == 0x12345678);
    auto item = FindSettingsItem("time", "standard_network_clock_sufficient_accuracy_minutes");
    assert(item && item->size == 4 && item->value == 43200);
    assert(FindSettingsItem("settings_debug", "is_debug_mode_enabled")->size == 1);
    assert(FindSettingsItem("hbloader", "applet_heap_size")->size == 8);
    assert(!FindSettingsItem("time", "unknown"));
    assert(!FindSettingsItem("unknown", "is_debug_mode_enabled"));
    std::filesystem::remove_all(directory);
    std::cout << "settings persistence, write failure, corrupt storage, IPC boundaries and item types: PASS\n";
}
