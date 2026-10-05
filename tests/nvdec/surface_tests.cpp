#include <iostream>
#include <soc.h>
#include <soc/host1x/classes/nvdec/output_surface.h>
#include <soc/host1x/classes/nvdec/surface_writer.h>
#include <soc/host1x/surface_writer.h>
#include <soc/host1x/classes/vic/surface_writer.h>
#include <soc/host1x/classes/nvdec/codecs/codec.h>
#include <soc/host1x/classes/nvdec/codecs/h264.h>
#include <fstream>
extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
}
using namespace skyline;
using namespace skyline::soc::host1x;
static void Check(bool condition,const char *what) { if(!condition) throw std::runtime_error(what); }
static size_t BlockOffset(u32 x,u32 y,u32 pitch,u32 gobs=2) {
 return (y/(8*gobs))*size_t(pitch/64)*512*gobs + (x/64)*512*gobs + ((y%(8*gobs))/8)*512 + (x%64/32)*256 + (y%8/2)*64 + (x%32/16)*32 + (y%2)*16 + x%16;
}
class PacketCodec : public nvdec::Codec {
 public:
    std::vector<u8> packet;
    size_t Pending() const { return submissions.size(); }
    void SetHidden(bool value) { hiddenFrame=value; }
    nvdec::OutputSurface output;
    PacketCodec(const DeviceState &s,const nvdec::Registers &r,nvdec::CodecId codec):Codec(s,r) { initialized=decoder.Initialize(codec); Check(initialized,"decoder initialize"); }
 private:
    span<const u8> ComposeBitstream() override { return packet; }
    u64 GetOutputLumaAddress() override { return output.luma[0]; }
    nvdec::OutputSurface GetOutputSurface() override { return output; }
};
static std::vector<u8> ReadFile(const char *path) {
 std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),{}};
}
static void DecodeTest(const char *h264,const char *vp9) {
 DeviceState state{std::make_shared<soc::HostSoc>()}; nvdec::Registers regs{};
 FrameQueue queue; queue.OpenStream(1); queue.OpenStream(2);
 PacketCodec a(state,regs,nvdec::CodecId::H264),b(state,regs,nvdec::CodecId::Vp9);
 a.packet=ReadFile(h264);
 auto ivf=ReadFile(vp9); Check(ivf.size()>44,"VP9 fixture"); u32 size{};std::memcpy(&size,ivf.data()+32,4);
 b.packet.assign(ivf.begin()+44,ivf.begin()+44+size);
 a.output={64,48,64,64,false,false,{0x10000,0},{0x20000,0}};
 b.output={64,48,64,64,false,false,{0x30000,0},{0x40000,0}};
 a.Decode(queue,1); b.Decode(queue,2);
 Check(state.soc->smmu.writes.size()==4,"each successful NVDEC decode must write both guest planes without VIC");
 auto fa=queue.PopPresentationFrame(0x10000),fb=queue.PopPresentationFrame(0x30000);
 Check(fa && fb,"NVDEC to VIC streams remain isolated");
 for(u32 y=0;y<48;y++) for(u32 x=0;x<64;x++) {
  Check(state.soc->smmu.bytes[0x10000+BlockOffset(x,y,64)]==fa->data[0][y*fa->linesize[0]+x],"H264 guest surface equals VIC frame");
  Check(state.soc->smmu.bytes[0x30000+BlockOffset(x,y,64)]==fb->data[0][y*fb->linesize[0]+x],"VP9 guest surface equals VIC frame");
 }
 // Decode-only output still materializes its surface but never reaches VIC.
 a.SetHidden(true);a.Decode(queue,1);a.SetHidden(false);
 Check(state.soc->smmu.writes.size()==6,"decode-only AVFrame writes guest surface");
 Check(!queue.PopPresentationFrame(0x10000),"decode-only AVFrame is not presented to VIC");
 // A VP9 non-show frame can be accepted without yielding an AVFrame.
 // Such submissions must not retain metadata forever.
 Check((b.packet[0]&3)==2,"VP9 profile-zero keyframe header");
 b.packet[0]&=~2;
 for(int i=0;i<128;i++) b.Decode(queue,2);
 Check(b.Pending()<=64,"dropped/non-output submission metadata is bounded");
 b.packet[0]|=2;b.Decode(queue,2);
 Check(queue.PopPresentationFrame(0x30000)!=nullptr,"visible output after dropped packets");
 // A non-show VP9 reference must also exist in guest memory even though
 // avcodec_receive_frame deliberately returns no presentation AVFrame.
 b.packet[0]&=~2;b.SetHidden(true);auto writeCount=state.soc->smmu.writes.size();
 b.Decode(queue,2);b.SetHidden(false);b.packet[0]|=2;
 Check(state.soc->smmu.writes.size()==writeCount+2,"hidden VP9 reference materializes without presentation");
 Check(!queue.PopPresentationFrame(0x30000),"hidden VP9 reference stays out of VIC");
 for(u32 y=0;y<48;y++) for(u32 x=0;x<64;x++)
  Check(state.soc->smmu.bytes[0x30000+BlockOffset(x,y,64)]==fb->data[0][y*fb->linesize[0]+x],"hidden VP9 reference pixels");
 Check(b.Pending()<=64,"hidden VP9 metadata retired");
 for(u32 y=0;y<24;y++) for(u32 x=0;x<32;x++) {
  Check(state.soc->smmu.bytes[0x40000+BlockOffset(x*2,y,64)]==fb->data[1][y*fb->linesize[1]+x],"hidden VP9 reference U pixels");
  Check(state.soc->smmu.bytes[0x40000+BlockOffset(x*2+1,y,64)]==fb->data[2][y*fb->linesize[2]+x],"hidden VP9 reference V pixels");
 }
 auto validPacket=b.packet;auto unchanged=state.soc->smmu.bytes;writeCount=state.soc->smmu.writes.size();
 b.SetHidden(true);b.packet[0]&=~2;b.packet.resize(32);b.Decode(queue,2);
 Check(state.soc->smmu.writes.size()==writeCount && state.soc->smmu.bytes==unchanged,"failed hidden decode must not expose unfinished buffers");
 b.SetHidden(false);b.packet=std::move(validPacket);
 // Reusing a destination is a second presentation event, not a cache hit.
 a.Decode(queue,1); a.Decode(queue,1);
 Check(queue.PopPresentationFrame(0x10000)!=nullptr && queue.PopPresentationFrame(0x10000)!=nullptr,"repeated surface queue entries");
 queue.CloseStream(1); queue.CloseStream(2);
}
static void ReorderTest(const char *path) {
 auto data=ReadFile(path);data.resize(data.size()+AV_INPUT_BUFFER_PADDING_SIZE);
 auto *parser=av_parser_init(AV_CODEC_ID_H264);auto *parseContext=avcodec_alloc_context3(nullptr);
 Check(parser && parseContext,"H264 parser");
 std::vector<std::vector<u8>> packets;const u8 *src=data.data();int remaining=int(data.size()-AV_INPUT_BUFFER_PADDING_SIZE);
 while(remaining>0) {
  u8 *packet{};int length{};int consumed=av_parser_parse2(parser,parseContext,&packet,&length,src,remaining,AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
  Check(consumed>0 || length>0,"parser progress");if(length)packets.emplace_back(packet,packet+length);src+=consumed;remaining-=consumed;
 }
 u8 *packet{};int length{};av_parser_parse2(parser,parseContext,&packet,&length,nullptr,0,AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
 if(length)packets.emplace_back(packet,packet+length);
 av_parser_close(parser);avcodec_free_context(&parseContext);
 DeviceState state{std::make_shared<soc::HostSoc>()};nvdec::Registers regs{};FrameQueue queue;queue.OpenStream(11);
 PacketCodec codec(state,regs,nvdec::CodecId::H264);nvdec::FfmpegDecoder reference;Check(reference.Initialize(nvdec::CodecId::H264),"reference decoder");
 std::vector<nvdec::OutputSurface> targets;size_t outputs{};
 for(size_t i=0;i<packets.size();i++) {
  // All packets reuse luma while pitches and chroma addresses vary. Delayed
  // output must still use the originating submission, never the current one.
  nvdec::OutputSurface dest{64,48,u32(64+(i%2)*64),u32(64+(i%3)*64),false,false,{0x10000,0},{0x20000+i*0x8000,0}};
  targets.push_back(dest);codec.output=dest;codec.packet=packets[i];codec.Decode(queue,11);
  Check(reference.SendPacket(packets[i],i+1),"reference packet");
  while(auto expected=reference.ReceiveFrame()) {
   auto token=size_t(expected->pts);Check(token && token<=targets.size(),"submission token");const auto &original=targets[token-1];
   auto actual=queue.PopPresentationFrame(original.luma[0]);Check(bool(actual),"reordered frame retained for VIC");outputs++;
   for(u32 y=0;y<48;y++) for(u32 x=0;x<64;x++) {
    Check(actual->data[0][y*actual->linesize[0]+x]==expected->data[0][y*expected->linesize[0]+x],"VIC reordered pixels");
    Check(state.soc->smmu.bytes[original.luma[0]+BlockOffset(x,y,original.lumaPitch)]==expected->data[0][y*expected->linesize[0]+x],"reordered luma pitch snapshot");
   }
   for(u32 y=0;y<24;y++) for(u32 x=0;x<32;x++)
    Check(state.soc->smmu.bytes[original.chroma[0]+BlockOffset(x*2,y,original.chromaPitch)]==expected->data[1][y*expected->linesize[1]+x],"reordered chroma destination snapshot");
  }
 }
 Check(outputs>=4 && outputs<packets.size(),"fixture exercised decoder delay");
}
static void GuestH264Test(const char *path) {
 DeviceState state{std::make_shared<soc::HostSoc>()};nvdec::Registers regs{};
 auto packet=ReadFile(path);nvdec::H264DecoderContext picture{};
 picture.streamLength=u32(packet.size());auto &p=picture.parameterSet;
 p.picWidthInMbs=4;p.frameHeightInMbs=3;p.frameMbsOnlyFlag=1;
 p.chromaFormatIdc=1;p.pitchLuma=64;p.pitchChroma=128;p.currPicIdx=4;
 p.lumaFrameOffset.raw=2;p.chromaFrameOffset.raw=3;
 std::fill(picture.weightScale4x4.begin(),picture.weightScale4x4.end(),16);
 std::fill(picture.weightScale8x8.begin(),picture.weightScale8x8.end(),16);
 regs.pictureInfoOffset.raw=0x700;regs.frameBitstreamOffset.raw=0x800;
 regs.surfaceLumaOffsets[4].raw=0x100;regs.surfaceChromaOffsets[4].raw=0x200;
 std::memcpy(state.soc->smmu.bytes.data()+0x70000,&picture,sizeof(picture));
 std::memcpy(state.soc->smmu.bytes.data()+0x80000,packet.data(),packet.size());
 FrameQueue queue;queue.OpenStream(13);nvdec::H264 codec(state,regs);codec.Decode(queue,13);
 Check(state.soc->smmu.writes.size()==2,"production H264 materializes register destinations");
 auto frame=queue.PopPresentationFrame(0x10200);Check(bool(frame),"production H264 retains VIC frame");
 for(u32 y=0;y<48;y++) for(u32 x=0;x<64;x++)
  Check(state.soc->smmu.bytes[0x10200+BlockOffset(x,y,64)]==frame->data[0][y*frame->linesize[0]+x],"production H264 picture-relative luma");
 for(u32 y=0;y<24;y++) for(u32 x=0;x<32;x++)
  Check(state.soc->smmu.bytes[0x20300+BlockOffset(x*2,y,128)]==frame->data[1][y*frame->linesize[1]+x],"production H264 picture-relative chroma");
}
static void CropTest(const char *path) {
 DeviceState state{std::make_shared<soc::HostSoc>()};nvdec::Registers regs{};FrameQueue queue;queue.OpenStream(12);
 PacketCodec codec(state,regs,nvdec::CodecId::H264);codec.packet=ReadFile(path);
 codec.output={64,64,64,64,false,false,{0x10000,0},{0x20000,0}};
 codec.Decode(queue,12);
 Check(state.soc->smmu.writes.size()==2,"cropped H264 writes the complete coded surface");
 auto visible=queue.PopPresentationFrame(0x10000);
 Check(visible && visible->width==64 && visible->height==50,"VIC retains the visible cropped frame");
 for(u32 y=0;y<50;y++) for(u32 x=0;x<64;x++)
  Check(state.soc->smmu.bytes[0x10000+BlockOffset(x,y,64)]==visible->data[0][y*visible->linesize[0]+x],"crop does not change visible pixels");
}
int main(int argc,char **argv) {
 if(argc>=3) {DecodeTest(argv[1],argv[2]);GuestH264Test(argv[1]);}
 if(argc>=4) ReorderTest(argv[3]);
 if(argc>=5) CropTest(argv[4]);
 DeviceState state{std::make_shared<soc::HostSoc>()};
 nvdec::Registers regs{}; nvdec::H264ParameterSet p{};
 p.picWidthInMbs=5; p.frameHeightInMbs=4; p.frameMbsOnlyFlag=1;
 p.pitchLuma=128; p.pitchChroma=192; p.currPicIdx=2;
 regs.surfaceLumaOffsets[2].raw=0x10; regs.surfaceChromaOffsets[2].raw=0x40;
 p.lumaFrameOffset.raw=3; p.chromaFrameOffset.raw=5;
 auto output=nvdec::GetH264OutputSurface(regs,p);
 Check(output.luma[0]==0x1300 && output.chroma[0]==0x4500,"picture-relative offsets");
 AVFrame *frame=av_frame_alloc(); frame->format=AV_PIX_FMT_YUV420P; frame->width=80; frame->height=64;
 Check(av_frame_get_buffer(frame,32)==0,"frame allocation");
 for(int y=0;y<64;y++) for(int x=0;x<80;x++) frame->data[0][y*frame->linesize[0]+x]=u8(x+3*y);
 for(int y=0;y<32;y++) for(int x=0;x<40;x++) { frame->data[1][y*frame->linesize[1]+x]=u8(17+x+y); frame->data[2][y*frame->linesize[2]+x]=u8(211-x-y); }
 nvdec::WriteDecodedSurface(state,output,frame);
 auto &mem=state.soc->smmu.bytes;
 for(u32 y=0;y<64;y++) for(u32 x=0;x<80;x++) Check(mem[0x1300+BlockOffset(x,y,128)]==u8(x+3*y),"NVDEC luma sample");
 for(u32 y=0;y<32;y++) for(u32 x=0;x<40;x++) {
  Check(mem[0x4500+BlockOffset(x*2,y,192)]==u8(17+x+y),"NV12 U sample");
  Check(mem[0x4500+BlockOffset(x*2+1,y,192)]==u8(211-x-y),"NV12 V sample");
 }

 // NV24 preserves full-resolution chroma and independent pitches.
 AVFrame *full=av_frame_alloc(); full->format=AV_PIX_FMT_YUV444P;full->width=80;full->height=64;
 Check(av_frame_get_buffer(full,32)==0,"NV24 frame allocation");
 for(int y=0;y<64;y++) for(int x=0;x<80;x++) {
  full->data[0][y*full->linesize[0]+x]=u8(x+y);
  full->data[1][y*full->linesize[1]+x]=u8(x+7*y);
  full->data[2][y*full->linesize[2]+x]=u8(255-x-y);
 }
 auto nv24=output;nv24.nv24=true;nv24.luma[0]=0x20000;nv24.chroma[0]=0x28000;
 nvdec::WriteDecodedSurface(state,nv24,full);
 for(u32 y=0;y<64;y++) for(u32 x=0;x<80;x++) {
  Check(mem[0x28000+BlockOffset(x*2,y,192)]==u8(x+7*y),"NV24 U sample");
  Check(mem[0x28000+BlockOffset(x*2+1,y,192)]==u8(255-x-y),"NV24 V sample");
 }
 av_frame_free(&full);
 // MBAFF/interlaced sequences may leave every field offset zero and use
 // a woven frame, as on the real nvtegra driver.
 p.frameMbsOnlyFlag=0;p.frameSurfaces=0;
 auto woven=nvdec::GetH264OutputSurface(regs,p);
 Check(!woven.fieldSurfaces && woven.luma[0]==0x1300,"zero field offsets select woven frame");
 nvdec::WriteDecodedSurface(state,woven,frame);
 // Split fields receive alternate rows of both decoded planes.
 p.frameMbsOnlyFlag=0;p.frameSurfaces=0;p.lumaTopOffset.raw=0x300;p.lumaBotOffset.raw=0x340;
 p.chromaTopOffset.raw=0x380;p.chromaBotOffset.raw=0x3c0;
 auto fields=nvdec::GetH264OutputSurface(regs,p);
 nvdec::WriteDecodedSurface(state,fields,frame);
 for(u32 field=0;field<2;field++) for(u32 y=0;y<32;y++) for(u32 x=0;x<80;x++)
  Check(mem[fields.luma[field]+BlockOffset(x,y,128)]==u8(x+3*(y*2+field)),"field luma parity");
 for(u32 field=0;field<2;field++) for(u32 y=0;y<16;y++) for(u32 x=0;x<40;x++)
  Check(mem[fields.chroma[field]+BlockOffset(x*2,y,192)]==u8(17+x+y*2+field),"field chroma parity");
 p.frameSurfaces=1;auto frameSurface=nvdec::GetH264OutputSurface(regs,p);
 Check(!frameSurface.fieldSurfaces && frameSurface.luma[0]==0x1300,"interlaced frame surface flag");
 // Aliased and partially overlapping field destinations are invalid.
 for(u64 delta : {0ULL,256ULL}) {
  auto overlapping=fields;overlapping.luma[1]=overlapping.luma[0]+delta;
  auto unchanged=mem;bool failed=false;
  try {nvdec::WriteDecodedSurface(state,overlapping,frame);} catch(const std::invalid_argument &) {failed=true;}
  Check(failed && mem==unchanged,"overlapping fields rejected before writes");
 }
 // Validate all destinations before touching any memory, including chroma.
 auto invalid=output;invalid.chromaPitch=64;auto before=mem;bool rejected=false;
 try { nvdec::WriteDecodedSurface(state,invalid,frame); } catch(const std::invalid_argument &) {rejected=true;}
 Check(rejected && mem==before,"invalid chroma pitch does not partially overwrite luma");
 invalid=output;invalid.chroma[0]=0xfffff000;rejected=false;
 try { nvdec::WriteDecodedSurface(state,invalid,frame); } catch(const std::invalid_argument &) {rejected=true;}
 Check(rejected && mem==before,"IOVA overflow does not truncate");
 invalid=output;invalid.chroma[0]=0x1000000;rejected=false;
 try { nvdec::WriteDecodedSurface(state,invalid,frame); } catch(const std::invalid_argument &) {rejected=true;}
 Check(rejected && mem==before,"unmapped chroma does not partially overwrite luma");
 p.currPicIdx=127;rejected=false;
 try {nvdec::GetH264OutputSurface(regs,p);} catch(const std::invalid_argument &) {rejected=true;}
 Check(rejected,"surface index bounds");
 // Negative FFmpeg strides must walk backwards from data, not cast to size_t.
 frame->data[0]+=63*frame->linesize[0];frame->linesize[0]=-frame->linesize[0];
 frame->data[1]+=31*frame->linesize[1];frame->linesize[1]=-frame->linesize[1];
 frame->data[2]+=31*frame->linesize[2];frame->linesize[2]=-frame->linesize[2];
 nvdec::WriteDecodedSurface(state,output,frame);
 for(u32 y=0;y<64;y++) for(u32 x=0;x<80;x++)
  Check(mem[0x1300+BlockOffset(x,y,128)]==u8(x+3*(63-y)),"negative source stride");
 // The existing VIC conversion still writes pitch and configurable block heights.
 frame->data[0]+=63*frame->linesize[0];frame->linesize[0]=-frame->linesize[0];
 frame->data[1]+=31*frame->linesize[1];frame->linesize[1]=-frame->linesize[1];
 frame->data[2]+=31*frame->linesize[2];frame->linesize[2]=-frame->linesize[2];
 vic::ConfigStruct config{};auto &out=config.outputSurfaceConfig;
 out.outLumaWidth=79;out.outLumaHeight=63;out.outChromaWidth=39;out.outChromaHeight=31;
 vic::PlaneOffsets offsets{};offsets.luma.raw=0x500;offsets.chromaU.raw=0x600;
 for(u32 layout=0;layout<2;layout++) {
  out.outBlkKind=layout ? vic::BlkKind::Generic16Bx2 : vic::BlkKind::Pitch;out.outBlkHeight=2;
  vic::WriteNv12Surface(state,config,offsets,frame);
  for(u32 y=0;y<64;y++) for(u32 x=0;x<80;x++)
   Check(mem[0x50000+(layout ? BlockOffset(x,y,128,4) : y*80+x)]==u8(x+3*y),"VIC layout preserved");
 }
 av_frame_free(&frame);
 std::cout<<"NVDEC output surface tests passed\n";
}
