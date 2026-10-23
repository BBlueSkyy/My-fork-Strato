// Compile the actual diagnostic hook bodies with the actual packet definitions.
#include <cassert>
#include <cstring>
#include <memory>
#include "gpu/diagnostic/sparking_upload_provenance.h"
using u8=std::uint8_t;using u16=std::uint16_t;using u32=std::uint32_t;using u64=std::uint64_t;
namespace gpu { namespace diagnostic=skyline::gpu::diagnostic; }
template<class T> struct Register {
    T value{};T &operator*(){return value;}T *operator->(){return &value;}
};
struct Channel { std::shared_ptr<int> asCtx=std::make_shared<int>(); };
struct Dma {
    struct Registers {
#pragma pack(push,1)
#include "dma_types.inc"
#pragma pack(pop)
        Register<LaunchDma> launchDma;
        Register<u64> offsetIn,offsetOut;
        Register<u32> lineLengthIn,lineCount,pitchIn,pitchOut,remapConstA,remapConstB;
        Register<RemapComponents> remapComponents;
        Register<Surface> srcSurface,dstSurface;
    } registers;
    Channel channelCtx;
    void Trace() {
#include "dma_trace.inc"
    }
};
struct I2m {
#pragma pack(push,1)
#include "i2m_type.inc"
#pragma pack(pop)
    Channel channelCtx;
    void Trace(RegisterState &state) {
#include "i2m_trace.inc"
    }
};
int main() {
    using namespace gpu::diagnostic;
    Dma dma;
    dma.registers.offsetOut.value=100;
    dma.registers.lineLengthIn.value=16;
    auto space=reinterpret_cast<std::uintptr_t>(dma.channelCtx.asCtx.get());
    SparkingUploads().enabled=false;dma.Trace();
    assert(SparkingTransfers().Snapshot(space,100,16).find("last_seq=0")!=std::string::npos);
    SparkingUploads().enabled=true;dma.Trace();
    assert(SparkingTransfers().Snapshot(space,100,16).find("envelope_bytes=16")!=std::string::npos);
    dma.registers.launchDma.value.multiLineEnable=true;
    dma.registers.launchDma.value.dstMemoryLayout=Dma::Registers::LaunchDma::MemoryLayout::Pitch;
    dma.registers.pitchOut.value=32;dma.registers.lineCount.value=3;
    dma.Trace();
    assert(SparkingTransfers().Snapshot(space,100,16).find("envelope_bytes=80")!=std::string::npos);
    I2m i2m;I2m::RegisterState state{};
    state.offsetOut=200;state.lineLengthIn=32;state.lineCount=2;state.pitchOut=64;
    state.launchDma.layout=I2m::RegisterState::DmaDstMemoryLayout::Pitch;i2m.Trace(state);
    space=reinterpret_cast<std::uintptr_t>(i2m.channelCtx.asCtx.get());
    auto report=SparkingTransfers().Snapshot(space,200,1);
    assert(report.find("kind=I2M")!=std::string::npos && report.find("envelope_bytes=96")!=std::string::npos);
}
