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
    assert(!cap.ReservePassSnapshot(99, 1024));
    assert(!cap.ReservePassSnapshot(0, 0));
    std::vector<std::thread> workers;
    // Five intervals of six exhaust the GLOBAL 30-snapshot cap.
    for (unsigned interval = 0; interval < 5; ++interval) {
        workers.emplace_back([&, interval] {
            for (unsigned i = 0; i < 6; ++i)
                assert(cap.ReservePassSnapshot(interval, 1024));
            assert(!cap.ReservePassSnapshot(interval, 1024));
        });
    }
    for (auto &worker : workers)
        worker.join();
    assert(!cap.ReservePassSnapshot(5, 1024));
    cap.Record("POSTPROCESS_TEST");
    cap.Finish();
    assert(!cap.active.load());
    std::ifstream in(path / "gpu_events.txt");
    std::string output((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    assert(output.find("POSTPROCESS_TEST") != std::string::npos);
    fs::remove_all(path);
}
