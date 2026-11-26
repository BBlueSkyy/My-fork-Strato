#pragma once
#include <common.h>
namespace skyline::gpu::texture {
struct Dimensions { u32 width,height,depth; explicit operator bool() const { return width && height && depth; } };
struct MipLevelLayout {
 Dimensions dimensions; size_t linearSize,targetLinearSize,blockLinearSize,blockHeight,blockDepth, tileWidthSpacing{};
 MipLevelLayout(Dimensions d,size_t l,size_t t,size_t b,size_t h,size_t z,size_t spacing=0):dimensions(d),linearSize(l),targetLinearSize(t),blockLinearSize(b),blockHeight(h),blockDepth(z),tileWidthSpacing(spacing){}
};
struct HostFormat { size_t blockWidth,blockHeight,bpb; size_t GetSize(u32 w,u32 h) const {return size_t(w)*h*bpb;} };
struct GuestTexture {Dimensions dimensions; const HostFormat *format; struct {size_t blockHeight,blockDepth;u32 pitch;size_t tileWidthSpacing{};} tileConfig;u32 mipLevelCount{1};};
size_t GetBlockLinearLayerSize(Dimensions,size_t,size_t,size_t,size_t,size_t);
void CopyPitchToBlockLinear(Dimensions,size_t,size_t,size_t,u32,size_t,size_t,u8*,u8*);
void CopyBlockLinearToPitch(Dimensions,size_t,size_t,size_t,u32,size_t,size_t,u8*,u8*);
}
