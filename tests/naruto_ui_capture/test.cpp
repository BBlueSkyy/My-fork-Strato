// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/gpu/diagnostic/naruto_ui_capture.h"
#include <cassert>
#include <filesystem>
#include <string>

int main() {
    namespace fs = std::filesystem;
    using skyline::gpu::diagnostic::UiFrameCapture;
    using skyline::gpu::diagnostic::IsWideTextStripGeometry;
    static_assert(IsWideTextStripGeometry(1184, 32));
    static_assert(IsWideTextStripGeometry(1184, 64));
    static_assert(IsWideTextStripGeometry(1664, 64));
    static_assert(IsWideTextStripGeometry(1664, 96));
    static_assert(IsWideTextStripGeometry(768, 96));
    static_assert(IsWideTextStripGeometry(704, 96));
    static_assert(UiFrameCapture::MaxPostFourthTextBytes >=
                  24ULL * 1600 * 900 * 4);
    static_assert(!IsWideTextStripGeometry(1024, 1024));
    static_assert(!IsWideTextStripGeometry(224, 64));
    static_assert(!IsWideTextStripGeometry(512, 10));
    static_assert(!IsWideTextStripGeometry(2049, 64));
    const fs::path root{fs::temp_directory_path() / "strato_naruto_ui_capture_regression"};
    fs::create_directories(root);
    static_assert(UiFrameCapture::PresentFrameCount == 24);
    static_assert(UiFrameCapture::FrameCount == 4);
    static_assert(UiFrameCapture::PresentFrameCount > UiFrameCapture::FrameCount);
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
    assert(capture.ClaimPreUiDraw(0));
    assert(!capture.ClaimPreUiDraw(0));
    assert(!capture.ClaimPreUiDraw(UiFrameCapture::FrameCount));
    assert(capture.ClaimPreUiDraw(1));
    assert(!capture.ClaimPreUiDraw(1));
    using TextFocus = UiFrameCapture::TextFocus;
    // All six snapshots per interval now belong to full-resolution draws:
    // pre/post destination and sampled source for text + compressed sprite.
    assert(!capture.ClaimTextFocus(0, TextFocus::GlyphOutput));
    assert(!capture.ClaimTextFocus(0, TextFocus::StripComposite));
    assert(capture.ClaimTextFocus(0, TextFocus::FullConsumer) == 1);
    assert(!capture.ClaimTextFocus(0, TextFocus::FullConsumer));
    assert(capture.ClaimTextFocus(0, TextFocus::FullSprite) == 1);
    assert(!capture.ClaimTextFocus(0, TextFocus::FullSprite));
    assert(!capture.ClaimTextFocus(UiFrameCapture::FrameCount, TextFocus::FullSprite));
    assert(capture.ClaimTextFocus(1, TextFocus::FullConsumer) == 1);
    assert(capture.ClaimPreSpriteDraw(0));
    assert(!capture.ClaimPreSpriteDraw(0));
    assert(!capture.ClaimPreSpriteDraw(UiFrameCapture::FrameCount));
    assert(capture.ClaimPreSpriteDraw(1));
    // The trace has one qualifying text-strip consumer per UI cycle.
    // Probe immediately BEFORE the first and only one, never a second.
    assert(!capture.ClaimUiBeforeText(0, 2822400));
    assert(capture.ClaimUiBeforeText(0xA100, 2822400) == 0);
    assert(!capture.ClaimUiBeforeText(0xA100, 2822400));
    capture.CompleteUiCompositorCycle(0xA100);
    for (std::uint32_t n{1}; n < UiFrameCapture::MaxUiMidpointSnapshots; ++n) {
        assert(capture.ClaimUiBeforeText(0xA100, 2822400) == n);
        assert(!capture.ClaimUiBeforeText(0xA100, 2822400));
        capture.CompleteUiCompositorCycle(0xA100);
    }
    assert(!capture.ClaimUiBeforeText(0xA100, 2822400)); // budget exhausted
    // Only the fourth actual text consumer may allocate a snapshot.
    // The 0->1 re-arm after the reverse Fermi boundary is exercised here.
    assert(!capture.ClaimPostFourthText(0, 2822400));
    auto probeFourth = [&](std::uintptr_t target, std::uint32_t expect) {
        assert(!capture.ClaimPostFourthText(target, 2822400)); // draw 1
        assert(!capture.ClaimPostFourthText(target, 2822400)); // draw 2
        assert(!capture.ClaimPostFourthText(target, 2822400)); // draw 3
        assert(capture.ClaimPostFourthText(target, 2822400) == expect); // draw 4
        assert(!capture.ClaimPostFourthText(target, 2822400)); // draw 5
        assert(!capture.ClaimPostFourthText(target, 2822400)); // draw 6
    };
    probeFourth(0xA100, 0);
    capture.CompleteUiCompositorCycle(0xA100);
    probeFourth(0xA100, 1);
    probeFourth(0xA200, 2);
    for (std::uint32_t n{3}; n < UiFrameCapture::MaxPostFourthTextSnapshots; ++n) {
        capture.CompleteUiCompositorCycle(0xA100);
        probeFourth(0xA100, n);
    }
    capture.CompleteUiCompositorCycle(0xA100);
    for (int j{}; j < 6; ++j)
        assert(!capture.ClaimPostFourthText(0xA100, 2822400)); // cap exhausted
    // Background-only Fermi source is never eligible. Register the
    // compositor's actual target as observed from a GPU consumer draw.
    assert(!capture.ReserveFermiSource(0, 0xF200, 2822400));
    assert(!capture.ReserveFermiSource(0xF100, 0xF100, 2822400));
    assert(!capture.ReserveFermiSource(0xF100, 0xF200, 2822400));
    capture.ObserveUiCompositorTarget(0xF100);
    capture.ObserveUiCompositorTarget(0xF100); // deduplicate observed texture
    assert(!capture.ReserveFermiSource(0xF200, 0xF100, 2822400)); // background
    for (std::uint32_t i{}; i < UiFrameCapture::MaxFermiSourceSnapshots; ++i) {
        assert(capture.ReserveFermiSource(0xF100, 0xF200, 2822400) == i);
        assert(!capture.ReserveFermiSource(0xF200, 0xF100, 2822400));
        assert(!capture.ReserveFermiSource(0xF100, 0xF300, 2822400));
    }
    assert(!capture.ReserveFermiSource(0xF100, 0xF200, 2822400));
    assert(capture.NextFocusedDrawId() == 1);
    assert(capture.NextFocusedDrawId() == 2);

    // Exactly one pre-destination, pre-source and post-destination for each
    // of the two consumer categories: six readbacks per presentation interval.
    for (std::uint32_t frame{}; frame < UiFrameCapture::FrameCount; ++frame) {
        for (std::uint32_t i{}; i < UiFrameCapture::PassSnapshotsPerInterval; ++i)
            assert(capture.ReservePassSnapshot(frame, 2822400));
        assert(!capture.ReservePassSnapshot(frame, 1));
    }
    assert(!capture.ReservePassSnapshot(0, 1)); // global snapshot count also exhausted
    assert(!capture.ReservePassSnapshot(UiFrameCapture::FrameCount, 1024));
    capture.Record("test_event=1");
    capture.Finish();
    assert(!capture.IsWatchedMapping(0x1000, 1));
    assert(fs::exists(root / "gpu_events.txt"));

    capture.Start(root.string());
    assert(!capture.ClaimPostFourthText(0xA100, 2822400));
    assert(!capture.ClaimPostFourthText(0xA100, 2822400));
    assert(!capture.ClaimPostFourthText(0xA100, 2822400));
    assert(capture.ClaimPostFourthText(0xA100, 2822400) == 0);

    assert(capture.ClaimUiBeforeText(0xA100, 2822400) == 0); // session reset
    assert(!capture.ClaimUiBeforeText(0xA100, 2822400));
    capture.CompleteUiCompositorCycle(0xA100);
    // New session MUST clear the former observed targets/direction.
    assert(!capture.ReserveFermiSource(0xF100, 0xF200, 2822400));
    capture.ObserveUiCompositorTarget(0xF200);
    assert(capture.ReserveFermiSource(0xF200, 0xF100, 2822400) == 0);
    assert(!capture.ReserveFermiSource(0xF100, 0xF200, 2822400));
    assert(!capture.IsWatchedMapping(0x1000, 1)); // no cross-session leakage
    assert(!capture.IsFlippedDestination(0, 0x3000, 0x100)); // flip registry reset
    assert(capture.ClaimPreUiDraw(0)); // one-shot ownership resets
    assert(!capture.ClaimTextFocus(0, TextFocus::GlyphOutput));
    assert(!capture.ClaimTextFocus(0, TextFocus::StripComposite));
    assert(capture.ClaimTextFocus(0, TextFocus::FullConsumer) == 1);
    assert(capture.ClaimTextFocus(0, TextFocus::FullSprite) == 1);
    assert(capture.NextFocusedDrawId() == 1);
    assert(capture.ClaimPreSpriteDraw(0));
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
