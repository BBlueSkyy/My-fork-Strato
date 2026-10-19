// SPDX-License-Identifier: MPL-2.0
// Android/Vulkan allocation is replaced; TIC cache lookup, translation, dirty
// tracking execute the complete production methods.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cerrno>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
namespace fmt { template<class T> auto ptr(T *pointer) { return pointer; } }
namespace skyline {
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
namespace constant { constexpr size_t PageSize=4096; }
template<class T,size_t Extent=std::dynamic_extent> struct span:std::span<T,Extent> {
    using std::span<T,Extent>::span;
    span(std::span<T,Extent> other):std::span<T,Extent>(other){}
    bool valid() const { return this->data()!=nullptr; }
    bool contains(span other) const {
        auto start=reinterpret_cast<uintptr_t>(this->data()), address=reinterpret_cast<uintptr_t>(other.data());
        return address>=start && address-start<=this->size_bytes() && other.size_bytes()<=this->size_bytes()-(address-start);
    }
    span<T> subspan(size_t offset,size_t count=std::dynamic_extent)const{return std::span<T,Extent>::subspan(offset,count);}
    span<T> first(size_t count)const{return std::span<T,Extent>::first(count);}
    template<class U> span<U> cast()const{return {reinterpret_cast<U*>(this->data()),this->size_bytes()/sizeof(U)};}
    void copy_from(span other){std::copy(other.begin(),other.end(),this->begin());}
};
template<class T> span(T*,size_t)->span<T>;
template<class... Args> struct exception:std::runtime_error { exception(const char *message,Args&&...):std::runtime_error(message){} };
template<class... Args> exception(const char*,Args&&...)->exception<Args...>;
using SharedSpinLock=std::shared_mutex;
namespace util {
template<class T> constexpr T DivideCeil(T x,T y){return (x+y-1)/y;}
template<class T,class U> constexpr T AlignDown(T x,U y){return x&~(y-1);}
template<class T,class U> constexpr T AlignUp(T x,U y){return (x+y-1)&~(y-1);}
template<class T> concept TrivialObject=std::is_trivially_copyable_v<T> && !requires(T v){v.data();};
template<class To,class From> constexpr To BitCast(const From &from){return std::bit_cast<To>(from);}
inline bool IsPageAligned(size_t value){return value%constant::PageSize==0;}
template<class T> struct ObjectHash {
    size_t operator()(const T &value)const {
        size_t hash{};const auto *bytes=reinterpret_cast<const u8*>(&value);
        for(size_t i{};i<sizeof(T);++i)hash=hash*131+bytes[i];
        return hash;
    }
};
}
}
#define TRACE_EVENT(...)
#include "segment_table.h"
#include "address_space.h"
#include "address_space.inc"
#include "dirty_tracking.h"
#include "tic.inc"
using namespace skyline;
namespace tsl {template<class K,class V,class H=std::hash<K>>using robin_map=std::unordered_map<K,V,H>;}
using VkBuffer=uintptr_t;using VkDeviceSize=u64;using VkFormat=int;
constexpr u32 VK_REMAINING_MIP_LEVELS=~u32{};
namespace vk {
enum class ImageViewType {e1D,e1DArray,e2D,e2DArray,e3D,eCube,eCubeArray};
using Format=int;
namespace raii {struct BufferView {};}
}
namespace Shader {enum class TextureType {Color1D,ColorArray1D,Color2D,ColorArray2D,Color3D,ColorCube,ColorArrayCube,Buffer};enum class ImageFormat {};}
namespace skyline::gpu {
namespace texture {
struct FormatInfo {
    int swizzleMapping{};
    u32 bpb{1};int vkFormat{};
    int Aspect(bool)const{return 1;}
};
using Format=const FormatInfo*;
enum class TileMode {Pitch,Block};
struct TileConfig {TileMode mode{};u32 pitch{};u8 blockHeight{},blockDepth{};};
struct Dimensions {u32 width{},height{},depth{};};
}
struct GuestTexture {
    std::vector<span<u8>> mappings;
    std::vector<bool> sparseMappings;
    size_t mappingOffset{};
    texture::Format format{};int aspect{},swizzle{};
    u32 baseArrayLayer{},mipLevelCount{},viewMipBase{},viewMipCount{},layerCount{};
    texture::Dimensions dimensions;
    texture::TileConfig tileConfig;
    vk::ImageViewType viewType{};
    size_t GetSize(){return constant::PageSize;}
#include "set_mappings.inc"
    bool MappingsValid()const;
};
#include "valid_mappings.inc"
struct Texture {bool replaced{};GuestTexture guest;};
struct TextureView {std::shared_ptr<Texture> texture;};
struct TextureManager {
    unsigned calls{};
    std::vector<std::shared_ptr<TextureView>> created;
    std::shared_ptr<TextureView> FindOrCreate(const GuestTexture &guest,u64) {
        ++calls;
        auto texture=std::make_shared<Texture>();texture->guest=guest;
        auto view=std::make_shared<TextureView>();view->texture=texture;
        created.push_back(view);return view;
    }
};
namespace interconnect {
namespace engine_common {struct TexHeaderPool {u64 offset{};u32 maximumIndex{};};}
struct DirtyManager {void Bind(dirty::Handle,const engine_common::TexHeaderPool&) {}};
struct CachedMappedBufferView {
    bool view{};
    void Update(auto&,u64,u64){view=true;}
};
using Gmmu=FlatMemoryManager<u64,0,20,12,16>;
struct AddressSpace {Gmmu gmmu;};
struct InterconnectContext {
    AddressSpace addressSpace;
    struct Channel {AddressSpace *asCtx;u64 channelSequenceNumber{};}channelCtx{&addressSpace};
    struct Gpu {TextureManager texture;}gpu;
    struct Executor {u64 tag{1};}executor;
    struct Memory {span<u8> base;}memory;
};
inline unsigned warnings{};
#define LOGW(...) (++warnings)
#include "classes.inc"
static texture::Format ConvertTicFormat(TextureImageControl::FormatWord word,bool) {
    static texture::FormatInfo format;
    return word.format==TextureImageControl::ImageFormat::R8 ? &format : nullptr;
}
static int ConvertTicSwizzleMapping(TextureImageControl::FormatWord,int){return 0;}
static bool IsIntegerTicFormat(texture::Format){return false;}
static std::optional<std::pair<vk::Format,u8>> ConvertShaderImageFormat(Shader::ImageFormat){return std::pair<vk::Format,u8>{0,1};}
static std::shared_ptr<TextureView> CreateNullTexture(InterconnectContext&) {
    return std::make_shared<TextureView>();
}
#include "methods.inc"
vk::raii::BufferView *Textures::GetOrCreateTexelBufferView(InterconnectContext&,CachedMappedBufferView&,vk::Format) {
    static vk::raii::BufferView view;return &view;
}
}
}
using namespace skyline::gpu;
using namespace skyline::gpu::interconnect;
constexpr size_t Page=constant::PageSize;
struct PhysicalMemory {
    int fd=memfd_create("strato-tic-test",MFD_CLOEXEC);
    span<u8> bytes;
    explicit PhysicalMemory(size_t pages) {
        assert(fd>=0 && !ftruncate(fd,pages*Page));
        auto *pointer=static_cast<u8*>(mmap(nullptr,pages*Page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));
        assert(pointer!=MAP_FAILED);bytes={pointer,pages*Page};
    }
    ~PhysicalMemory(){assert(!munmap(bytes.data(),bytes.size()));close(fd);}
};
TextureImageControl Tic(u64 address) {
    TextureImageControl tic{};
    tic.formatWord.format=TextureImageControl::ImageFormat::R8;
    tic.addressLow=address;tic.addressHigh=address>>32;
    tic.textureType=TextureImageControl::TextureType::e2D;
    tic.headerType=TextureImageControl::HeaderType::Pitch;
    return tic;
}
void CheckRemapping(bool storeOnly) {
    InterconnectContext ctx;PhysicalMemory physical(4);ctx.memory.base=physical.bytes;
    auto &mm=ctx.addressSpace.gmmu;
    constexpr u64 Pool=Page,Data=Page*8;
    *physical.bytes.cast<TextureImageControl>().data()=Tic(Data);
    physical.bytes.cast<TextureImageControl>()[1]=Tic(Data);
    mm.Map(Pool,physical.bytes.data(),Page);mm.Map(Data,physical.bytes.data()+Page,Page);
    DirtyManager manager;engine_common::TexHeaderPool registers{Pool,0};
    Textures textures(manager,{registers});
    u32 index{};
    auto get=[&]{return textures.GetTexture(ctx,index,Shader::TextureType::Color2D);};
    auto first=get();assert(first->texture && ctx.gpu.texture.calls==1);
    auto boundary=[&]{++ctx.channelCtx.channelSequenceNumber;textures.MarkAllDirty();};
    boundary();assert(get()==first && ctx.gpu.texture.calls==1); // Submit alone preserves both caches.
    if(storeOnly){registers.maximumIndex=1;boundary();assert(get()==first && ctx.gpu.texture.calls==1);index=1;}
    mm.Map(Data,physical.bytes.data()+Page*2,Page);boundary();
    auto remapped=get();
    assert(remapped->texture->guest.mappings.front().data()==physical.bytes.data()+Page*2);
    assert(ctx.gpu.texture.calls==2);
    mm.MapSparse(Data,Page);boundary();
    assert(get()->texture->guest.sparseMappings.front());assert(ctx.gpu.texture.calls==3);
    mm.Map(Data,physical.bytes.data()+Page*3,Page);boundary();
    assert(get()->texture->guest.mappings.front().data()==physical.bytes.data()+Page*3);
    assert(ctx.gpu.texture.calls==4);
    const auto before=warnings;mm.Unmap(Data,Page);boundary();
    assert(!get()->texture && warnings==before+1 && ctx.gpu.texture.calls==4);
    mm.MapSparse(Data,Page);boundary();
    assert(get()->texture->guest.sparseMappings.front() && warnings==before+1);
}
void CheckAddressSpaceIdentity() {
    InterconnectContext ctx;AddressSpace other;PhysicalMemory physical(3);ctx.memory.base=physical.bytes;
    constexpr u64 Pool=Page,Data=Page*8;
    *physical.bytes.cast<TextureImageControl>().data()=Tic(Data);
    ctx.addressSpace.gmmu.Map(Pool,physical.bytes.data(),Page);
    ctx.addressSpace.gmmu.Map(Data,physical.bytes.data()+Page,Page);
    other.gmmu.Map(Pool,physical.bytes.data(),Page);other.gmmu.Map(Data,physical.bytes.data()+Page*2,Page);
    assert(ctx.addressSpace.gmmu.GetMappingGeneration()==other.gmmu.GetMappingGeneration());
    DirtyManager manager;engine_common::TexHeaderPool registers{Pool,0};Textures textures(manager,{registers});
    assert(textures.GetTexture(ctx,0,Shader::TextureType::Color2D)->texture->guest.mappings.front().data()==physical.bytes.data()+Page);
    ctx.channelCtx.asCtx=&other; // Same generation and channel sequence.
    assert(textures.GetTexture(ctx,0,Shader::TextureType::Color2D)->texture->guest.mappings.front().data()==physical.bytes.data()+Page*2);
    assert(ctx.gpu.texture.calls==2);
}
void CheckFragmentedPool() {
    InterconnectContext ctx;PhysicalMemory physical(4);ctx.memory.base=physical.bytes;
    constexpr u64 Pool=Page,Data=Page*8;
    auto &mm=ctx.addressSpace.gmmu;
    *physical.bytes.subspan(Page*2).cast<TextureImageControl>().data()=Tic(Data);
    mm.Map(Pool,physical.bytes.data(),Page);mm.Map(Pool+Page,physical.bytes.data()+Page*2,Page);
    mm.Map(Data,physical.bytes.data()+Page*3,Page);
    DirtyManager manager;engine_common::TexHeaderPool registers{Pool,Page/sizeof(TextureImageControl)};
    Textures textures(manager,{registers});
    auto *view=textures.GetTexture(ctx,registers.maximumIndex,Shader::TextureType::Color2D);
    assert(view->texture && view->texture->guest.mappings.front().data()==physical.bytes.data()+Page*3);
    const auto before=warnings;
    assert(textures.GetTextureType(ctx,registers.maximumIndex)==Shader::TextureType::Color2D);
    assert(!textures.IsTexturePixelFormatInteger(ctx,registers.maximumIndex));
    CachedMappedBufferView cached;
    assert(textures.GetTextureBufferView(ctx,registers.maximumIndex,cached));
    assert(textures.GetImageBufferView(ctx,registers.maximumIndex,Shader::ImageFormat{},cached));
    assert(warnings==before);
}
void CheckPoolStorage() {
    InterconnectContext ctx;PhysicalMemory physical(3);ctx.memory.base=physical.bytes;
    constexpr u64 Pool=Page;
    auto &mm=ctx.addressSpace.gmmu;
    mm.Map(Pool,physical.bytes.data(),Page);mm.Map(Pool+Page,physical.bytes.data()+Page*2,Page);
    DirtyManager manager;engine_common::TexHeaderPool registers{Pool+32,Page/sizeof(TextureImageControl)};
    TexturePoolState::EngineRegisters engine{registers};
    physical.bytes.subspan(Page*2+32).cast<TextureImageControl>()[0]=Tic(Page*9);
    {
        dirty::ManualDirtyState<TexturePoolState> pool(manager,engine);
        auto *header=pool.UpdateGet(ctx).GetHeader(ctx,registers.maximumIndex);
        assert(reinterpret_cast<u8*>(header)==physical.bytes.data()+Page*2+32 && header->Iova()==Page*9);
        physical.bytes.subspan(Page*2+32).cast<TextureImageControl>()[0]=Tic(Page*10);
        assert(header->Iova()==Page*10); // Disjoint resident descriptors remain direct aliases.
        physical.bytes.subspan(Page+32).cast<TextureImageControl>()[0]=Tic(Page*11);
        mm.Map(Pool+Page,physical.bytes.data()+Page,Page);
        header=pool.UpdateGet(ctx).GetHeader(ctx,registers.maximumIndex); // Refresh without a manual dirty notification.
        assert(header->Iova()==Page*11);
        pool.MarkDirty(true);
        assert(pool.UpdateGet(ctx).GetHeader(ctx,registers.maximumIndex)==header);
    }
    registers.maximumIndex=0;
    {
        bool dirty{};TexturePoolState pool{dirty::Handle{&dirty},manager,engine};
        pool.Flush(ctx);
        assert(reinterpret_cast<u8*>(pool.textureHeaders.data())==physical.bytes.data()+32);
        pool.PurgeCaches();assert(physical.bytes[32]==0); // Contiguous common path aliases directly.
    }
}
void CheckSplitDescriptor() {
    InterconnectContext ctx;PhysicalMemory physical(3);ctx.memory.base=physical.bytes;
    constexpr u64 Pool=Page,Data=Page*8;
    auto &mm=ctx.addressSpace.gmmu;mm.Map(Pool,physical.bytes.data(),Page);mm.Map(Pool+Page,physical.bytes.data()+Page*2,Page);
    auto tic=Tic(Data);std::memcpy(physical.bytes.data()+Page-16,&tic,16);
    std::memcpy(physical.bytes.data()+Page*2,reinterpret_cast<u8*>(&tic)+16,16);
    DirtyManager manager;engine_common::TexHeaderPool registers{Pool+Page-16,0};
    dirty::ManualDirtyState<TexturePoolState> pool(manager,TexturePoolState::EngineRegisters{registers});
    auto *header=pool.UpdateGet(ctx).GetHeader(ctx,0);
    assert(header && !std::memcmp(header,&tic,sizeof(tic)));
    mm.MapSparse(Pool+Page,Page);header=pool.UpdateGet(ctx).GetHeader(ctx,0);
    assert(header && header->Iova()==Data && !std::memcmp(reinterpret_cast<u8*>(header)+16,std::array<u8,16>{}.data(),16));
    const auto before=warnings;mm.Unmap(Pool+Page,Page);
    assert(!pool.UpdateGet(ctx).GetHeader(ctx,0) && warnings==before+1);
}
void CheckDescriptorMappingKinds() {
    InterconnectContext ctx;PhysicalMemory physical(3);ctx.memory.base=physical.bytes;
    constexpr u64 Pool=Page,Data=Page*8;
    auto &mm=ctx.addressSpace.gmmu;
    mm.Map(Pool,physical.bytes.data(),Page);mm.MapSparse(Pool+Page,Page);
    mm.Map(Data,physical.bytes.data()+Page*2,Page);
    DirtyManager manager;engine_common::TexHeaderPool registers{Pool,Page/sizeof(TextureImageControl)};
    Textures textures(manager,{registers});
    auto get=[&]{return textures.GetTexture(ctx,registers.maximumIndex,Shader::TextureType::Color2D);};
    const auto before=warnings;
    assert(!get()->texture && warnings==before && ctx.gpu.texture.calls==0); // Sparse descriptors read zero.
    *physical.bytes.subspan(Page).cast<TextureImageControl>().data()=Tic(Data);
    mm.Map(Pool+Page,physical.bytes.data()+Page,Page);
    assert(get()->texture && warnings==before && ctx.gpu.texture.calls==1);
    mm.Unmap(Pool+Page,Page);
    assert(!get()->texture && warnings==before+1 && ctx.gpu.texture.calls==1);
    mm.MapSparse(Pool+Page,Page);
    assert(!get()->texture && warnings==before+1 && ctx.gpu.texture.calls==1);
}
void CheckUnusedUnmappedTail() {
    InterconnectContext ctx;PhysicalMemory physical(2);ctx.memory.base=physical.bytes;
    constexpr u64 Pool=Page,Data=Page*8;
    *physical.bytes.cast<TextureImageControl>().data()=Tic(Data);
    auto &mm=ctx.addressSpace.gmmu;mm.Map(Pool,physical.bytes.data(),Page);mm.Map(Data,physical.bytes.data()+Page,Page);
    DirtyManager manager;engine_common::TexHeaderPool registers{Pool,Page/sizeof(TextureImageControl)};
    Textures textures(manager,{registers});const auto before=warnings;
    assert(textures.GetTexture(ctx,0,Shader::TextureType::Color2D)->texture && warnings==before);
    assert(!textures.GetTexture(ctx,registers.maximumIndex,Shader::TextureType::Color2D)->texture && warnings==before+1);
}
int main(int argc,char **argv) {
    if(argc==1 || std::string(argv[1])=="cache") {CheckRemapping(false);CheckRemapping(true);CheckAddressSpaceIdentity();}
    if(argc==1 || std::string(argv[1])=="pool") {CheckUnusedUnmappedTail();CheckFragmentedPool();CheckPoolStorage();CheckSplitDescriptor();CheckDescriptorMappingKinds();}
    std::cout<<"PASS: production TIC translation/cache regressions\n";
}
