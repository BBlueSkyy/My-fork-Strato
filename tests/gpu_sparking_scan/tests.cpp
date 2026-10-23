#include <cstdlib>
#include "gpu/diagnostic/sparking_frame_capture.h"
#include "gpu/diagnostic/sparking_upload_provenance.h"
#include <cassert>
#include <filesystem>
#include <fstream>
int main() {
    using namespace skyline::gpu::diagnostic;
    auto root = std::filesystem::path(std::getenv("SPARKING_TEST_ROOT")) / "policy";
    std::filesystem::create_directories(root / "capture_001");
    std::ofstream(root / "ENABLE_HDR_DRAW_SCAN");
    SparkingFrameCapture session;
    session.Start((root / "capture_001").string());
    assert(session.hdrDrawScan.load());
    for (unsigned i=1;i<=192;++i) {
        auto draw=session.ClaimHdrDraw(0x1234);
        assert(draw && *draw==i);
        assert(!session.ClaimHdrDraw(0x5678));
        assert(!session.ClaimHdrDraw(0x1234,1));
    }
    assert(!session.ClaimHdrDraw(0x1234));
    assert(!session.ReservePassSnapshot(0,32,SparkingFocusRole::SceneHdr));
    session.Finish();
    session.Start((root / "capture_001").string());
    assert(session.ClaimHdrDraw(0x5678)==1);
    assert(session.ReserveScanBytes(384ULL*1024*1024));
    assert(!session.ReserveScanBytes(1));
    session.Finish();
    session.ConfigureShaderDump(root);
    session.DumpShader(0x1234,true,std::array<std::uint8_t,4>{3,2,35,7});
    const auto first=std::distance(std::filesystem::directory_iterator(root/"scan_shaders"),std::filesystem::directory_iterator{});
    session.DumpShader(0x1234,true,std::array<std::uint8_t,4>{3,2,35,7});
    assert(first==std::distance(std::filesystem::directory_iterator(root/"scan_shaders"),std::filesystem::directory_iterator{}));
    std::filesystem::remove(root / "ENABLE_HDR_DRAW_SCAN");
    session.Start((root / "capture_001").string());
    assert(!session.hdrDrawScan.load());
    assert(!session.ClaimHdrDraw(0x1234));
    assert(session.ReservePassSnapshot(0,32,SparkingFocusRole::SceneHdr));
    session.Finish();
    assert(IsSparkingCubeGeometry(128,128,1,6,8));
    assert(!IsSparkingCubeGeometry(64,64,1,6,8));
    auto &uploads=SparkingUploads();
    uploads.enabled=false;
    assert(!uploads.Reserve(32));
    uploads.enabled=true;
    {
        auto proof=uploads.Reserve(SparkingUploadProvenance::MaxBytes);
        assert(proof && !uploads.Reserve(1));
    }
    assert(uploads.UsedBytes()==0);
    auto proof=uploads.Reserve(12);
    proof->guest={1,2,3,4};proof->linear={1,2,3,4};proof->upload={1,2,3,4};
    proof->slices.push_back({0,0,0,4,0,4,0,4});
    std::array<std::uint8_t,4> changed{1,2,9,4};
    assert(SparkingUploadProof::FirstDifference(proof->upload,changed)==2);
    assert(SparkingUploadProof::Crc(proof->upload)!=SparkingUploadProof::Crc(changed));
    assert(proof->Write(root/"proof",changed,"fixture\n"));
    std::ifstream report(root/"proof.txt");
    const std::string contents{std::istreambuf_iterator<char>(report),{}};
    assert(contents.find("equal=0 first_difference=2")!=std::string::npos);
    proof.reset();assert(uploads.UsedBytes()==0);
    // Retain creation bytes independently from later writes and the upload.
    std::array<std::uint8_t,4> original{0,126,0,60};
    auto creation=uploads.CaptureCreation(original);
    assert(creation && creation->guest[0]==0);
    original[0]=42;
    assert(creation->guest[0]==0);
    proof=uploads.Reserve(12);
    proof->guest={42,126,0,60};proof->linear=proof->guest;proof->upload=proof->guest;
    proof->creation=creation;
    assert(proof->Write(root/"origin",proof->upload,"fixture\n"));
    std::ifstream originRaw(root/"origin_creation_guest.raw",std::ios::binary);
    assert(originRaw.get()==0);
    proof.reset();creation.reset();assert(uploads.UsedBytes()==0);

    // Only the same address space and overlapping destination envelopes match.
    SparkingTransferTrace history;
    SparkingTransferTrace::Event event{};
    event.addressSpace=1;event.destination=100;event.bytes=20;event.kind="DMA";
    history.Record(event);
    assert(history.Snapshot(1,110,4).find("transfer seq=1")!=std::string::npos);
    assert(history.Snapshot(2,110,4).find("transfer seq=")==std::string::npos);
    assert(history.Snapshot(1,120,4).find("transfer seq=")==std::string::npos);
    event.destination=~std::uint64_t{0}-3;event.bytes=8;
    history.Record(event);
    assert(history.Snapshot(1,~std::uint64_t{0}-1,1).find("transfer seq=2")!=std::string::npos);
    for (size_t i=0;i<SparkingTransferTrace::Capacity;++i) history.Record(event);
    auto truncated=history.Snapshot(1,100,20);
    assert(truncated.find("overwritten=2")!=std::string::npos);
    assert(truncated.find("transfer seq=")==std::string::npos);
    assert(SparkingPitchExtent(16,3,32)==80);
    assert(SparkingPitchExtent(16,0,32)==0);
    assert(SparkingBlockExtent(1024,128,1,16,1)==131072);
    assert(SparkingPitchExtent(~std::uint64_t{0},2,32)==~std::uint64_t{0});
    auto stats=SparkingHdrStats::Decode({0,0,0,0});
    assert(stats.nanPixels==0 && stats.infPixels==0);
    // Red 11-bit NaN: exponent=31 and nonzero mantissa. Green infinity.
    stats=SparkingHdrStats::Decode({0xC1,0x07,0,0});
    assert(stats.nanPixels==1 && stats.infPixels==0);
    stats=SparkingHdrStats::Decode({0,0,0x3E,0});
    assert(stats.nanPixels==0 && stats.infPixels==1);
}
