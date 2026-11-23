// SPDX-License-Identifier: GPL-2.0-or-later
// Offline GraphicsEnvironment replay. Captured callbacks fail closed.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <shader_compiler/environment.h>
#include <shader_compiler/common/log.h>
#include <shader_compiler/common/settings.h>
#include <shader_compiler/host_translate_info.h>
#include <shader_compiler/frontend/maxwell/translate_program.h>
#include <shader_compiler/backend/spirv/emit_spirv.h>
namespace Shader::Log {
void Debug(const std::string&s){std::cerr<<s<<'\n';} void Warn(const std::string&s){std::cerr<<s<<'\n';} void Error(const std::string&s){std::cerr<<s<<'\n';}
}
class CaptureEnvironment final:public Shader::Environment {
    std::vector<char> code;
    std::map<std::pair<u32,u32>,u32> cbuf;
    std::map<u32,u32> types,integer,compares;
    u32 texture_buffer{},viewport{};
    u32 lookup(const std::map<u32,u32>&m,u32 handle,const char*kind){auto i=m.find(handle);if(i==m.end())throw std::runtime_error(fmt::format("uncaptured {} handle {:08X}",kind,handle));return i->second;}
public:
    Shader::RuntimeInfo runtime{};
    CaptureEnvironment(const char*binary,const char*metadata){
        std::ifstream f(binary,std::ios::binary);if(!f)throw std::runtime_error("missing Maxwell binary");code.assign(std::istreambuf_iterator<char>(f),{});
        static_assert(sizeof(Shader::ProgramHeader)==0x50);
        if(code.size()<sizeof(sph))throw std::runtime_error("truncated Maxwell header");std::memcpy(&sph,code.data(),sizeof(sph));
        stage=Shader::Stage::Fragment;
        std::ifstream m(metadata);if(!m)throw std::runtime_error("missing metadata");
        u32 early{},flip{},alpha{},alpha_func{}; if(!(m>>start_address>>texture_buffer>>viewport>>early>>flip>>alpha>>alpha_func>>runtime.alpha_test_reference))throw std::runtime_error("truncated metadata header");
        runtime.force_early_z=early;runtime.y_negate=flip;if(alpha)runtime.alpha_test_func=static_cast<Shader::CompareFunction>(alpha_func);
        for(auto&v:gp_passthrough_mask)if(!(m>>v))throw std::runtime_error("truncated passthrough mask");
        is_propietary_driver=texture_buffer==2;
        // Previous-stage interface is not needed for predicate control flow; all
        // inputs are enabled in this deliberately stated offline profile.
        runtime.previous_stage_stores.mask.set();
        auto record=[](auto& table,u32 key,u32 value){auto[it,ok]=table.emplace(key,value);if(!ok&&it->second!=value)throw std::runtime_error("conflicting captured metadata");};
        std::string tag;u32 a{},b{},c{};
        while(m>>tag){if(!(m>>a>>b))throw std::runtime_error("truncated metadata row");if(tag=="cbuf"){if(!(m>>c))throw std::runtime_error("truncated CBUF row");auto [it,ok]=cbuf.emplace(std::pair{a,b},c);if(!ok&&it->second!=c)throw std::runtime_error("conflicting captured CBUF reads");}else if(tag=="type")record(types,a,b);else if(tag=="integer")record(integer,a,b);else if(tag=="compare")record(compares,a,b);else throw std::runtime_error("unknown metadata record");}
    }
    u64 ReadInstruction(u32 address)override{if(address<start_address||size_t(address-start_address)+8>code.size())throw std::runtime_error("uncaptured instruction");u64 v;std::memcpy(&v,code.data()+address-start_address,8);return v;}
    u32 ReadCbufValue(u32 index,u32 offset)override{auto i=cbuf.find({index,offset});if(i==cbuf.end())throw std::runtime_error(fmt::format("uncaptured compile-time CBUF {}:{:X}",index,offset));std::cerr<<fmt::format("CBUF {}:{:X}={:08X}\n",index,offset,i->second);return i->second;}
    Shader::TextureType ReadTextureType(u32 h)override{return static_cast<Shader::TextureType>(lookup(types,h&0xFFFFF,"texture type"));}
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32)override{throw std::runtime_error("unexpected pixel-format callback (production GraphicsEnvironment also throws)");}
    bool IsTexturePixelFormatInteger(u32 h)override{return lookup(integer,h&0xFFFFF,"integer format");}
    Shader::CompareFunction ReadTextureCompareFunc(u32 h)override{return static_cast<Shader::CompareFunction>(lookup(compares,h,"compare"));}
    u32 ReadViewportTransformState()override{return viewport;} u32 TextureBoundBuffer()const override{return texture_buffer;}
    u32 LocalMemorySize()const override{return static_cast<u32>(sph.LocalMemorySize())+sph.common3.shader_local_memory_crs_size;}
    u32 SharedMemorySize()const override{return 0;}std::array<u32,3>WorkgroupSize()const override{return{0,0,0};}
    bool HasHLEMacroState()const override{return false;}
    std::optional<Shader::ReplaceConstant>GetReplaceConstBuffer(u32 bank,u32 offset)override{if(bank!=0||!is_propietary_driver)return{};switch(offset){case 0x640:return Shader::ReplaceConstant::BaseVertex;case 0x644:return Shader::ReplaceConstant::BaseInstance;case 0x648:return Shader::ReplaceConstant::DrawID;default:return{};}}
    void Dump(u64)override{}
};
int main(int argc,char**argv)try{
    if(argc!=4)throw std::runtime_error("usage: replay MAXWELL METADATA OUTPUT_PREFIX");
    CaptureEnvironment env(argv[1],argv[2]);Shader::ObjectPool<Shader::IR::Inst>ip;Shader::ObjectPool<Shader::IR::Block>bp;Shader::ObjectPool<Shader::Maxwell::Flow::Block>fp;
    Shader::Maxwell::Flow::CFG cfg(env,fp,Shader::Maxwell::Location{static_cast<u32>(env.StartAddress()+sizeof(Shader::ProgramHeader))});
    Shader::HostTranslateInfo host{};host.support_snorm_render_buffer=true;host.min_ssbo_alignment=16;
    Shader::Settings::values={.renderer_debug=false,.disable_shader_loop_safety_checks=false,.resolution_info={.active=false}};
    auto program=Shader::Maxwell::TranslateProgram(ip,bp,env,cfg,host);
    std::string prefix(argv[3]);std::ofstream(prefix+".ir")<<Shader::IR::DumpProgram(program);
    std::ofstream desc(prefix+".descriptors");
    for(size_t i=0;i<program.info.texture_descriptors.size();++i){auto&d=program.info.texture_descriptors[i];desc<<fmt::format("texture={} cbuf={} offset=0x{:X} count={} type={}\n",i,d.cbuf_index,d.cbuf_offset,d.count,static_cast<u32>(d.type));}
    Shader::Profile profile{};profile.supported_spirv=0x10400;profile.unified_descriptor_binding=true;profile.support_descriptor_aliasing=true;profile.support_derivative_control=true;
    Shader::Backend::Bindings bindings{};auto spv=Shader::Backend::SPIRV::EmitSPIRV(profile,env.runtime,program,bindings);
    std::ofstream f(prefix+".spv",std::ios::binary);f.write(reinterpret_cast<const char*>(spv.data()),spv.size()*4);
    std::cout<<fmt::format("SPIRV bytes={} textures={}\n",spv.size()*4,program.info.texture_descriptors.size());
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
