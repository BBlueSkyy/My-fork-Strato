// SPDX-License-Identifier: MPL-2.0
// Host-only RE5 diagnostic budget and trace sampling checks.
#include <gpu/diagnostic/re5_scene_capture.h>
#include <cassert>
#include <cstdint>
#include <filesystem>

int main() {
    namespace fs = std::filesystem;
    using skyline::gpu::diagnostic::SceneFrameCapture;
    const auto directory{fs::temp_directory_path() / "strato_re5_scene_capture_test"};
    fs::create_directories(directory);
    SceneFrameCapture capture;
    capture.Start(directory.string());

    for (std::uint32_t i = 0; i < 200; ++i) {
        const auto ordinal{capture.SampleSceneDraw(true)};
        const bool expected{i < 8 || i % 32 == 0};
        assert(ordinal.has_value() == expected);
        if (ordinal)
            assert(*ordinal == i);
    }
    for (std::uint32_t i = 0; i < 260; ++i) {
        const auto ordinal{capture.SampleSceneDraw(false)};
        const bool expected{i < 4 || i % 128 == 0};
        assert(ordinal.has_value() == expected);
        if (ordinal)
            assert(*ordinal == i);
    }
    for (std::uint32_t i = 0; i < SceneFrameCapture::FrameCount; ++i) {
        capture.presentIndex.store(i, std::memory_order_release);
        assert(capture.SampleSceneDraw(true).has_value() == (i != 0));
        for (std::uint32_t n = 0; n < 6; ++n)
            assert(capture.ReservePassSnapshot(i, 1024).has_value());
        assert(!capture.ReservePassSnapshot(i, 1024));
    }
    assert(!capture.ReservePassSnapshot(4, 1024));
    assert(!capture.ReservePassSnapshot(0, 0));
    capture.Record("SCENE_DRAW unit_test");
    capture.Finish();
    assert(!capture.SampleSceneDraw(true));
    assert(fs::exists(directory / "gpu_events.txt"));
    fs::remove_all(directory);
}
