#include "gpu/texture/origin_capture.h"
#include <cassert>
#include <sstream>
#include <string>
using namespace skyline::gpu::texture::diagnostic;

int main() {
    OriginCapture capture;
    std::array<uint8_t, 49152> mapping{};
    capture.Start(false);
    capture.Watch(mapping.data(),mapping.size());
    assert(!capture.Interested(mapping.data(),4096));
    assert(!capture.Enabled());
    lifecycle.Start(true);
    capture.Start(true);
    capture.Watch(mapping.data(),mapping.size());
    capture.Watch(mapping.data(),mapping.size()); // Deduplicated.
    assert(capture.Interested(mapping.data()+4096,4096));
    assert(!capture.Interested(mapping.data()+mapping.size(),1));

    Snapshot cube{};
    cube.id=42; cube.address=reinterpret_cast<uintptr_t>(mapping.data());
    cube.bytes=mapping.size(); cube.hostFormat=122; cube.width=32; cube.height=32;
    cube.layers=6; cube.levels=5; cube.generation=1;
    mapping[0]=8;
    assert(capture.Save(OriginStage::CreatedGuest,cube,mapping.data(),mapping.size()));
    assert(!capture.Save(OriginStage::CreatedGuest,cube,mapping.data(),mapping.size()));

    std::array<uint8_t, 32736> staging{};
    staging[0]=1; staging[24576]=2; // mip0 face0 and mip1 face0
    assert(capture.Save(OriginStage::StagingBeforeUpload,cube,staging.data(),staging.size()));
    bool deferredExecuted{};
    assert(capture.SaveDeferred(OriginStage::HostAfterUpload,cube,[&] {
        deferredExecuted=true;
        return std::vector<uint8_t>(staging.begin(),staging.end());
    }));
    assert(!deferredExecuted);
    assert(lifecycle.SetPhase(1));
    assert(capture.Save(OriginStage::SampledHost,cube,staging.data(),staging.size()));
    assert(!capture.CanRecord(OriginStage::SampledHost,cube)); // Same writer serial.
    cube.writeSerial=1;
    assert(capture.CanRecord(OriginStage::SampledHost,cube));
    assert(capture.Save(OriginStage::SampledHost,cube,staging.data(),staging.size()));
    cube.writeSerial=2;
    assert(!capture.CanRecord(OriginStage::SampledHost,cube)); // Two per phase.
    std::ostringstream out;
    assert(capture.Export(out));
    assert(deferredExecuted);
    const auto text=out.str();
    assert(text.find("# MK8ORIGIN1")!=std::string::npos);
    assert(text.find("host_after_upload,0,")!=std::string::npos);
    assert(text.find("sampled_host,1,")!=std::string::npos);
    assert(text.find("state,flags,detail,mip,face")!=std::string::npos);
    assert(text.find(",0,0,4096,1,")!=std::string::npos);
    assert(text.find(",1,0,1024,1,")!=std::string::npos);
    assert(!capture.Enabled());

    OriginCapture invalid;
    invalid.Start(true);
    invalid.Watch(mapping.data(),mapping.size());
    assert(invalid.SaveDeferred(OriginStage::HostAfterUpload,cube,
        [] { return std::vector<uint8_t>{}; }));
    std::ostringstream invalidOut;
    assert(invalid.Export(invalidOut));
    assert(invalidOut.str().find("invalid_deferred")!=std::string::npos);
}
