// SPDX-License-Identifier: MPL-2.0
#include <gpu/diagnostic/ue4_frame_capture.h>
#include <gpu/diagnostic/ue4_upload_provenance.h>
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
    const std::uint8_t fixtureShader[]{0x03, 0x02, 0x23, 0x07};
    cap.RememberTargetShader(0x7161B14045F710B5ULL, true,
                             fixtureShader, sizeof(fixtureShader));
    cap.Start(path.string());
    assert(cap.active.load());
    assert(fs::file_size(path / "shader_7161B14045F710B5.spv") == sizeof(fixtureShader));
    assert(cap.ClaimMaterialInputs());
    assert(!cap.ClaimMaterialInputs());

    // Capture only the three draws responsible for the large observed
    // increase in finite HDR values, not earlier shader-group boundaries.
    cap.presentIndex.store(2);
    for (std::uint32_t ordinal = 1; ordinal <= 200; ++ordinal) {
        auto selected = cap.ClaimHdrDraw();
        const bool expected = ordinal == 19 || ordinal == 20 || ordinal == 21;
        assert(bool(selected) == expected);
        if (selected)
            assert(*selected == ordinal);
    }
    cap.presentIndex.store(3);
    for (std::uint32_t ordinal = 1; ordinal <= 21; ++ordinal) {
        const auto selected = cap.ClaimHdrDraw();
        assert(bool(selected) == (ordinal >= 19));
        if (selected)
            assert(*selected == ordinal);
    }
    cap.presentIndex.store(4);
    assert(!cap.ClaimHdrDraw()); // third interval is excluded
    assert(cap.ClaimConversion());
    assert(!cap.ClaimConversion());
    cap.presentIndex.store(5);
    assert(cap.ClaimConversion());

    cap.Start(path.string()); // all focused draw state resets on re-arming
    assert(cap.ClaimMaterialInputs());
    assert(!cap.ClaimMaterialInputs());
    for (std::uint32_t ordinal = 1; ordinal < 19; ++ordinal)
        assert(!cap.ClaimHdrDraw());
    assert(cap.ClaimHdrDraw().value() == 19);
    assert(cap.ClaimHdrDraw().value() == 20);
    assert(cap.ClaimHdrDraw().value() == 21);

    // Separate focus quota: no more than 64 concurrent claims, even if
    // different executors try to reserve at the same time.
    std::vector<std::thread> workers;
    for (int n = 0; n < 8; ++n) {
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
    assert(!cap.ClaimMaterialInputs());
    assert(!cap.ReserveFocusedImage(1));

    std::ifstream in(path / "gpu_events.txt");
    std::string output((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    assert(output.find("POSTPROCESS_TEST") != std::string::npos);
    // Last-upload retention is explicitly enabled, globally bounded, and
    // releases its budget when the final snapshot owner goes away.
    using namespace skyline::gpu::diagnostic;
    Ue4UploadProvenance uploads;
    assert(!uploads.Reserve(8));
    uploads.enabled = true;
    auto full = uploads.Reserve(Ue4UploadProvenance::MaxBytes);
    assert(full && !uploads.Reserve(1));
    full.reset();
    auto proof = uploads.Reserve(12);
    assert(proof);
    proof->guest = {1, 2, 3, 4};
    proof->linear = proof->guest;
    proof->upload = proof->linear;
    proof->recorded = true;
    proof->slices = {{0, 0, 0, 2, 0, 2, 0, 2}, {0, 1, 2, 2, 2, 2, 2, 2}};
    const std::uint8_t after[]{1, 2, 9, 4};
    const auto prefix = path / "proof";
    assert(UploadProof::Crc(proof->upload) == 0xB63CFBCD);
    assert(UploadProof::FirstDifference(proof->upload, after) == 2);
    assert(UploadProof::FirstDifference(proof->upload, proof->upload) == 4);
    assert(UploadProof::FirstDifference(proof->upload, std::span(after, 2)) == 2);
    assert(proof->Write(prefix, after, "test_current=1\n"));
    assert(fs::file_size(prefix.string() + "_upload.raw") == 4);
    std::ifstream manifest(prefix.string() + ".txt");
    std::string comparison((std::istreambuf_iterator<char>(manifest)), {});
    assert(comparison.find("equal=1 first_difference=2") != std::string::npos);
    assert(comparison.find("equal=0 first_difference=0") != std::string::npos);
    // Invalid subresource extents are reported without out-of-bounds reads.
    proof->slices[1].hostBytes = 99;
    assert(proof->Write(prefix, after, ""));
    std::ifstream invalidManifest(prefix.string() + ".txt");
    comparison.assign(std::istreambuf_iterator<char>(invalidManifest), {});
    assert(comparison.find("invalid_slice mip=0 face=1") != std::string::npos);
    fs::remove_all(path);
}
