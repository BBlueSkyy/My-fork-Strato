#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
void check(bool v,const char*m){if(!v)throw std::runtime_error(m);}
using PackedPipelineState=int;
#ifdef OLD_VARIANT_CACHE
template<class T>using Variants=std::unique_ptr<T>;
#else
template<class T>using Variants=std::vector<std::unique_ptr<T>>;
#endif
template<class V>size_t variantCount(const V&v){if constexpr(requires{v.size();})return v.size();else return v?1:0;}

namespace engine{constexpr size_t PipelineCount=1;}
struct ShaderBinary{};
struct ConstantBufferSet{};
struct Samplers{};
struct Textures{int component{};};
struct GPU{};
struct InterconnectContext{GPU gpu;};
struct PipelineStateBundle{void Reset(int){}};
struct RuntimeGraphicsPipelineStateAccessor{
 Textures&textures;
 RuntimeGraphicsPipelineStateAccessor(std::unique_ptr<PipelineStateBundle>,InterconnectContext&,Textures&t,Samplers&,ConstantBufferSet&,const std::array<ShaderBinary,1>&):textures(t){}
};
namespace graphics{
struct Pipeline{
 int sourcePackedState,component;
 std::array<Pipeline*,4>transitionCache{};
 Pipeline(GPU&,RuntimeGraphicsPipelineStateAccessor&a,int packed):sourcePackedState(packed),component(a.textures.component){}
 bool MatchesTextureTypes(InterconnectContext&,Textures&t,ConstantBufferSet&)const{return component==t.component;}
 Pipeline*LookupNext(const PackedPipelineState&,InterconnectContext&,Textures&,ConstantBufferSet&);
};
struct PipelineManager{
 std::map<int,Variants<Pipeline>>map;
 Pipeline*FindOrCreate(InterconnectContext&,Textures&,Samplers&,ConstantBufferSet&,const PackedPipelineState&,const std::array<ShaderBinary,1>&);
};
#include "graphics.inc"
}
namespace compute{
struct Pipeline{
 int sourcePackedState,component;
 Pipeline(InterconnectContext&,Textures&t,Samplers&,ConstantBufferSet&,int p,const ShaderBinary&):sourcePackedState(p),component(t.component){}
 bool MatchesTextureTypes(InterconnectContext&,Textures&t,ConstantBufferSet&)const{return component==t.component;}
 bool CanBindDynamicDescriptors(InterconnectContext&,Textures&,Samplers&)const{return true;}
};
struct PipelineManager{
 std::map<int,Variants<Pipeline>>map;
 Pipeline*FindOrCreate(InterconnectContext&,Textures&,Samplers&,ConstantBufferSet&,const PackedPipelineState&,const ShaderBinary&);
};
#include "compute.inc"
}
int main(){
 InterconnectContext ctx;Textures textures;Samplers samplers;ConstantBufferSet buffers;std::array<ShaderBinary,1> shaders;
 graphics::PipelineManager graphics;compute::PipelineManager compute;
 std::array<graphics::Pipeline*,3>g{};std::array<compute::Pipeline*,3>c{};
 for(int type=0;type<3;++type){textures.component=type;g[type]=graphics.FindOrCreate(ctx,textures,samplers,buffers,1,shaders);c[type]=compute.FindOrCreate(ctx,textures,samplers,buffers,1,shaders[0]);}
 check(variantCount(graphics.map.at(1))==3&&variantCount(compute.map.at(1))==3,"float/uint/sint require independent variants");
 for(int type:{2,0,1,2,1,0}){textures.component=type;check(graphics.FindOrCreate(ctx,textures,samplers,buffers,1,shaders)==g[type],"graphics variant must survive unordered type reuse");check(compute.FindOrCreate(ctx,textures,samplers,buffers,1,shaders[0])==c[type],"compute variant must survive unordered type reuse");}
 for(int type=0;type<3;++type)g[0]->transitionCache[type]=g[type];
 for(int type=0;type<3;++type){textures.component=type;check(g[0]->LookupNext(1,ctx,textures,buffers)==g[type],"same-state transition must respect numeric specialization");}
 check(variantCount(graphics.map.at(1))==3&&variantCount(compute.map.at(1))==3,"repeated draws must not compile duplicate variants");
 std::cout<<"PASS graphics/compute typed variant reuse and cached transitions\n";
}
