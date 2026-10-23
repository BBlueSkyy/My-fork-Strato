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
    // The last virtual-page writer candidate survives the 4096-event ring.
    SparkingTransferTrace persistent;
    SparkingTransferTrace::Event early{};
    early.kind="DMA";early.addressSpace=0x1010;
    early.destination=0x5622C0000ULL;early.source=0xABC000;early.bytes=1081344;
    persistent.Record(early);
    for (int i=0;i<50000;++i) {
        early.destination=0x400000000ULL+static_cast<u64>(i%32)*65536;
        early.bytes=32;
        persistent.Record(early);
    }
    auto origin=persistent.Snapshot(0x1010,0x5622C0000ULL,1081344);
    assert(origin.find("transfer_matches=0")!=std::string::npos);
    assert(origin.find("last_writer_page=0x5622c0000 seq=1 kind=DMA")!=std::string::npos);
    assert(origin.find("last_writer_page=0x5623c0000 seq=1 kind=DMA")!=std::string::npos);
    early.addressSpace=0x2020;early.destination=0x5622C0000ULL;
    persistent.Record(early);
    assert(persistent.Snapshot(0x1010,0x5622C0000ULL,8).find(
        "last_writer_page=0x5622c0000 seq=1 kind=DMA")!=std::string::npos);
    persistent.Record({"I2M",0x1010,4,0x5622C0000ULL,4});
    assert(persistent.Snapshot(0x1010,0x5622C0000ULL,8).find(
        "last_writer_page=0x5622c0000 seq=50003 kind=I2M")!=std::string::npos);
    // Oversized or malformed envelopes cannot cause unbounded page scanning.
    SparkingTransferTrace capped;
    capped.Record({"DMA",0x1010,0,0x5622C0000ULL,UINT64_MAX});
    assert(capped.Snapshot(0x1010,0x5622C0000ULL,8).find(
        "capped_commands=1")!=std::string::npos);

}
