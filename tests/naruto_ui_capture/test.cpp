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

    assert(!capture.IsFlippedDestination(0, 0x2000, 0x100));
    capture.MarkFlippedDestination(0, 0x2000, 0x100);
    assert(capture.IsFlippedDestination(0, 0x2000, 0x100));
    assert(capture.IsFlippedDestination(0, 0x20FF, 1));
    assert(!capture.IsFlippedDestination(0, 0x2100, 1));
    assert(!capture.IsFlippedDestination(0, 0x1FFF, 1));
    assert(!capture.IsFlippedDestination(1, 0x2000, 0x100)); // no cross-interval leakage
    capture.MarkFlippedDestination(1, 0x4000, 0x100);
    assert(capture.IsFlippedDestination(1, 0x4000, 0x100));
    assert(!capture.IsFlippedDestination(1, 0x2000, 0x100));
    capture.MarkFlippedDestination(0, 0x3000, 0x100); // late flip overwrites old candidate
    assert(!capture.IsFlippedDestination(0, 0x2000, 0x100));
    assert(capture.IsFlippedDestination(0, 0x3000, 0x100));
    assert(!capture.ClaimPreUiDraw(0)); // too early for a late-draw snapshot
    assert(!capture.ClaimPreUiDraw(UiFrameCapture::FrameCount));
    assert(!capture.ClaimPreUiDraw(1)); // separate interval also starts gated
    assert(!capture.ShouldProbeUiPreDraw(0));
    for (std::uint32_t i{1}; i < 96; ++i)
        assert(!capture.ProbeUiDraw(0));
    assert(capture.ShouldProbeUiPreDraw(0));
    assert(capture.ClaimPreUiDraw(0));
    assert(!capture.ClaimPreUiDraw(0));
    assert(!capture.ClaimPreUiDraw(UiFrameCapture::FrameCount));
    assert(capture.ClaimPreUiDraw(1));
    assert(!capture.ClaimPreUiDraw(1));
    using TextFocus = UiFrameCapture::TextFocus;
    assert(capture.ClaimTextFocus(0, TextFocus::GlyphOutput) == 1);
    assert(capture.ClaimTextFocus(0, TextFocus::GlyphOutput) == 2);
    assert(!capture.ClaimTextFocus(0, TextFocus::GlyphOutput));
    assert(capture.ClaimTextFocus(0, TextFocus::StripComposite) == 1);
    assert(!capture.ClaimTextFocus(0, TextFocus::StripComposite));
    assert(capture.ClaimTextFocus(0, TextFocus::FullConsumer) == 1);
    assert(!capture.ClaimTextFocus(0, TextFocus::FullConsumer));
    assert(!capture.ClaimTextFocus(UiFrameCapture::FrameCount, TextFocus::GlyphOutput));
    assert(capture.ClaimTextFocus(1, TextFocus::GlyphOutput) == 1);

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
    assert(!capture.IsFlippedDestination(0, 0x3000, 0x100)); // flip registry reset
    assert(capture.ClaimPreUiDraw(0)); // one-shot ownership resets
    assert(capture.ClaimTextFocus(0, TextFocus::GlyphOutput) == 1);
    assert(capture.ClaimTextFocus(0, TextFocus::StripComposite) == 1);
    assert(capture.ReserveAtlasSnapshot(0, 0x1234, 8192));
    assert(!capture.ReserveAtlasSnapshot(0, 0x1234, 8192)); // deduplicated
    assert(!capture.ReserveAtlasSnapshot(0, 0, 8192));
    assert(!capture.ReserveAtlasSnapshot(UiFrameCapture::FrameCount, 0x99, 8192));
    assert(capture.ReserveAtlasSnapshot(0, 0x5678, 8192)); // second eligible atlas
    assert(!capture.ReserveAtlasSnapshot(0, 0x9ABC, 8192)); // per-frame atlas cap: 2
    assert(!capture.ReserveAtlasSnapshot(1, 0x1234, 8192)); // across intervals
    assert(capture.ReservePassSnapshot(0, 5760000)); // pass budget still shared
    capture.Finish();
    fs::remove_all(root);
}
