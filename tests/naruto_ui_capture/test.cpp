// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/gpu/diagnostic/naruto_ui_capture.h"
#include <cassert>
#include <filesystem>
#include <string>

int main() {
    namespace fs = std::filesystem;
    using skyline::gpu::diagnostic::UiFrameCapture;
    const fs::path root{fs::temp_directory_path() / "strato_naruto_ui_capture_regression"};
    fs::create_directories(root);
    UiFrameCapture capture;
    capture.Start(root.string());

    capture.WatchMapping(0x1000, 0x100);
    capture.WatchMapping(0x1000, 0x100);
    assert(capture.IsWatchedMapping(0x1000, 1));
    assert(capture.IsWatchedMapping(0x10FF, 1));
    assert(capture.IsWatchedMapping(0x0FFF, 2));
    assert(!capture.IsWatchedMapping(0x1100, 1));
    assert(!capture.IsWatchedMapping(0x0FFF, 1));
    assert(!capture.IsWatchedMapping(0, 8));

    for (std::uint32_t i{}; i < UiFrameCapture::PassSnapshotsPerInterval; ++i)
        assert(capture.ReservePassSnapshot(0, 5760000));
    assert(!capture.ReservePassSnapshot(0, 5760000));
    for (std::uint32_t frame{1}; frame < UiFrameCapture::FrameCount; ++frame)
        assert(capture.ReservePassSnapshot(frame, 5760000));
    assert(!capture.ReservePassSnapshot(UiFrameCapture::FrameCount, 1024));
    capture.Record("test_event=1");
    capture.Finish();
    assert(!capture.IsWatchedMapping(0x1000, 1));
    assert(fs::exists(root / "gpu_events.txt"));

    capture.Start(root.string());
    assert(!capture.IsWatchedMapping(0x1000, 1)); // no cross-session leakage
    assert(capture.ReservePassSnapshot(0, 5760000)); // budget reset
    capture.Finish();
    fs::remove_all(root);
}
