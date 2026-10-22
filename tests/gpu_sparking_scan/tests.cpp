#include <cstdlib>
#include "gpu/diagnostic/sparking_frame_capture.h"
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
    auto stats=SparkingHdrStats::Decode({0,0,0,0});
    assert(stats.nanPixels==0 && stats.infPixels==0);
    // Red 11-bit NaN: exponent=31 and nonzero mantissa. Green infinity.
    stats=SparkingHdrStats::Decode({0xC1,0x07,0,0});
    assert(stats.nanPixels==1 && stats.infPixels==0);
    stats=SparkingHdrStats::Decode({0,0,0x3E,0});
    assert(stats.nanPixels==0 && stats.infPixels==1);
}
