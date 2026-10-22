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
    assert(uploads.UsedBytes() == 0);
    assert(std::string(UploadRetentionStatusName(UploadRetentionStatus::NeverObserved)) == "never_observed");
    assert(std::string(UploadRetentionStatusName(UploadRetentionStatus::BudgetRejected)) == "budget_rejected");
    assert(std::string(UploadRetentionStatusName(UploadRetentionStatus::OutsideFocusedCubemap)) == "outside_focused_cubemap");
    assert(IsFocusedCubeGeometry(128, 128, 1, 6, 8));
    assert(!IsFocusedCubeGeometry(1, 1, 1, 6, 1));
    assert(!IsFocusedCubeGeometry(128, 128, 1, 6, 7));
    assert(!IsFocusedCubeGeometry(256, 128, 1, 6, 8));
    assert(!IsFocusedCubeGeometry(128, 128, 1, 1, 8));
    assert(std::string(UploadRetentionStatusName(UploadRetentionStatus::UploadRecorded)) == "upload_recorded");
    uploads.enabled = true;
    auto full = uploads.Reserve(Ue4UploadProvenance::MaxBytes);
    assert(full && !uploads.Reserve(1));
    assert(uploads.UsedBytes() == Ue4UploadProvenance::MaxBytes);
    full.reset();
    assert(uploads.UsedBytes() == 0);
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
    // Host-only first HDR anomaly scan: the marker lives in the parent
    // gpu_capture/ue4 directory, whereas Start() receives capture_001.
    const auto sessionDir{path / "capture_001"};
    fs::create_directories(sessionDir);
    cap.Start(sessionDir.string());
    assert(!cap.HdrFirstBadMode()); // absent marker keeps legacy behavior
    cap.Finish();
    std::ofstream(path / "ENABLE_HDR_FIRST_BAD").close();
    cap.Start(sessionDir.string());
    assert(cap.HdrFirstBadMode());
    assert(!cap.ClaimMaterialInputs());
    assert(!cap.ClaimConversion());
    cap.presentIndex.store(2);
    assert(!cap.ClaimHdrDraw(0));
    auto first = cap.ClaimHdrDraw(0x1111);
    assert(first && *first == 1);
    for (std::uint32_t ordinal = 2; ordinal <= 32; ++ordinal) {
        assert(!cap.ClaimHdrDraw(0x2222)); // another backing must not steal ordinals
        auto selected = cap.ClaimHdrDraw(0x1111);
        assert(selected && *selected == ordinal);
    }
    assert(!cap.ClaimHdrDraw(0x1111));
    cap.presentIndex.store(3);
    assert(!cap.ClaimHdrDraw(0x1111)); // only the first target/interval

    // 2x1 pixels: zero, then +Inf in red on the second pixel (R11 E=31).
    const std::uint8_t baseline[8]{};
    const std::uint8_t abnormal[8]{0, 0, 0, 0, 0xC0, 0x07, 0, 0};
    cap.RecordHdrScan(0, 2, 1, baseline, sizeof(baseline));
    cap.RecordHdrScan(1, 2, 1, abnormal, sizeof(abnormal));
    std::ifstream report(sessionDir / "hdr_first_suspect.txt");
    const std::string reportText((std::istreambuf_iterator<char>(report)), {});
    assert(reportText.find("first_new_nonfinite_draw=1") != std::string::npos);
    cap.Finish();
    fs::remove(path / "ENABLE_HDR_FIRST_BAD");

    // The upload-provenance retention mode must be mutually exclusive.
    std::ofstream(path / "ENABLE_HDR_FIRST_BAD").close();
    std::ofstream(path / "ENABLE_UPLOAD_PROVENANCE").close();
    cap.Start(sessionDir.string());
    assert(!cap.HdrFirstBadMode());
    cap.Finish();
    fs::remove_all(path);
}
