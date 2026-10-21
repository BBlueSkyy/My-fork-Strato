// SPDX-License-Identifier: MPL-2.0
#include <gpu/diagnostic/ue4_frame_capture.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "strato_ue4_capture_budget";
    fs::create_directories(path);
    skyline::gpu::diagnostic::Ue4FrameCapture cap;
    cap.Start(path.string());
    assert(cap.active.load());

    // Draw selection is per approximate presentation interval.
    for (std::uint32_t ordinal = 1; ordinal <= 200; ++ordinal) {
        auto selected = cap.ClaimHdrDraw();
        const bool expected = ordinal == 1 || ordinal == 32 ||
                              ordinal == 128 || ordinal == 200;
        assert(bool(selected) == expected);
        if (selected)
            assert(*selected == ordinal);
    }
    assert(cap.ClaimConversion());
    assert(!cap.ClaimConversion());
    cap.presentIndex.store(1);
    assert(cap.ClaimConversion());
    assert(cap.ClaimHdrDraw().value() == 1);

    // Separate focus quota: no more than 48 concurrent claims, even if
    // different executors try to reserve at the same time.
    std::vector<std::thread> workers;
    for (int n = 0; n < 6; ++n) {
        workers.emplace_back([&] {
            for (int i = 0; i < 8; ++i)
                assert(cap.ReserveFocusedImage(1024));
        });
    }
    for (auto &worker : workers)
        worker.join();
    assert(!cap.ReserveFocusedImage(1024));
    assert(!cap.ReserveFocusedImage(0));

    // Legacy render-pass quota remains independently bounded.
    assert(!cap.ReservePassSnapshot(99, 1024));
    assert(!cap.ReservePassSnapshot(0, 0));
    cap.Record("POSTPROCESS_TEST");
    cap.Finish();
    assert(!cap.active.load());
    assert(!cap.ClaimHdrDraw());
    assert(!cap.ClaimConversion());
    assert(!cap.ReserveFocusedImage(1));

    std::ifstream in(path / "gpu_events.txt");
    std::string output((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    assert(output.find("POSTPROCESS_TEST") != std::string::npos);
    fs::remove_all(path);
}
