// SPDX-License-Identifier: MPL-2.0
// Host-only RE5 diagnostic budget and trace sampling checks.
#include <gpu/diagnostic/re5_scene_capture.h>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>

int main() {
    namespace fs = std::filesystem;
    using skyline::gpu::diagnostic::SceneFrameCapture;
    using skyline::gpu::diagnostic::SceneExtentMatches;
    assert(SceneExtentMatches(1280, 720, 1280, 720));
    // The old gate rejects matched 1080p attachments; the new Docked files
    // have no internal geometry to prove why their scene probes were absent.
    assert(SceneExtentMatches(1920, 1080, 1920, 1080));
    assert(SceneExtentMatches(960, 540, 960, 540));
    assert(!SceneExtentMatches(1920, 1080, 1280, 720));
    assert(!SceneExtentMatches(1920, 1080, 1920, 720));
    assert(!SceneExtentMatches(0, 720, 0, 720));
    assert(!SceneExtentMatches(1280, 0, 1280, 0));
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
    for (std::uint32_t i = 0; i < 20; ++i) {
        const std::array<std::uint32_t, 6> geometry{1920 + i, 1080, 1, 1920 + i, 1080, 1};
        capture.RecordSceneGeometry(geometry);
        capture.RecordSceneGeometry(geometry);
    }
    capture.Finish();
    assert(!capture.SampleSceneDraw(true));
    assert(fs::exists(directory / "gpu_events.txt"));
    std::ifstream eventsFile(directory / "gpu_events.txt");
    const std::string events{std::istreambuf_iterator<char>{eventsFile}, {}};
    std::size_t geometryRows{}, offset{};
    while ((offset = events.find("SCENE_GEOMETRY", offset)) != std::string::npos) {
        ++geometryRows;
        ++offset;
    }
    assert(geometryRows == 16);
    assert(events.find("reserved_binding_probes=0") != std::string::npos);
    capture.Start(directory.string());
    std::uint32_t largeSnapshots{};
    constexpr std::size_t fullHdBytes{1920 * 1080 * 4};
    for (std::uint32_t interval = 0; interval < SceneFrameCapture::FrameCount; ++interval)
        for (std::uint32_t i = 0; i < SceneFrameCapture::PassSnapshotsPerInterval; ++i)
            largeSnapshots += capture.ReservePassSnapshot(interval, fullHdBytes).has_value();
    assert(largeSnapshots == SceneFrameCapture::MaxPassCaptureBytes / fullHdBytes);
    assert(!capture.ReservePassSnapshot(3, fullHdBytes));
    capture.Finish();
    fs::remove_all(directory);
}
