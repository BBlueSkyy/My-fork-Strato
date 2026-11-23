#!/usr/bin/env python3
"""Round-trip the actual PCHE bundle implementation, including numeric metadata."""
import os
import ctypes.util
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
common = root / 'app/src/main/cpp/skyline/gpu/interconnect/common'
with tempfile.TemporaryDirectory(prefix='texel-bundle-') as tmp:
    out = Path(tmp)
    header = (common/'pipeline_state_bundle.h').read_text()
    header = '\n'.join(line for line in header.splitlines() if not line.startswith(('#include','#pragma')))
    cpp = (common/'pipeline_state_bundle.cpp').read_text()
    cpp = '\n'.join(line for line in cpp.splitlines() if not line.startswith('#include'))
    facade = r'''
#include <algorithm>
#include <numeric>
#include <fstream>
#include <span>
#include <vector>
#include <stdexcept>
#include <cassert>
#include <shader_compiler/shader_info.h>
#include <shader_compiler/runtime_info.h>
namespace ranges = std::ranges;
extern "C" unsigned long long XXH64(const void*,size_t,unsigned long long);
template<class... A> std::runtime_error exception(const char* message,A...) { return std::runtime_error(message); }
namespace skyline {
template<class T> class span : public std::span<T> {
public:
 using std::span<T>::span;
 template<class R> span(R& r) : std::span<T>(r.data(),r.size()) {}
 auto subspan(size_t off,size_t count=std::dynamic_extent) const { auto s=std::span<T>::subspan(off,count);return span<T>{s.data(),s.size()}; }
 template<class U> auto cast() const {return span<U>{reinterpret_cast<U*>(this->data()),this->size_bytes()/sizeof(U)};}
 template<class U> U& as() const {return *reinterpret_cast<U*>(this->data());}
 template<class R> void copy_from(const R& r) {assert(this->size_bytes()==r.size()*sizeof(*r.data()));std::memcpy(this->data(),r.data(),this->size_bytes());}
};
template<class T> span(std::vector<T>&)->span<T>;
namespace gpu::interconnect {struct ShaderBinary {span<u8> binary; u32 baseOffset;};}
}
'''
    cache_source=(root/'app/src/main/cpp/skyline/gpu/pipeline_cache_manager.cpp').read_text()
    start=cache_source.index('    struct PipelineCacheFileHeader {');end=cache_source.index('    void PipelineCacheManager::Run()',start)
    cache_header=cache_source[start:end]
    cache_header=cache_header.replace('util::MakeMagic<u32>("PCHE")','u32{0x45484350}')
    main = r'''
int main(int argc,char**argv) {
 using namespace skyline::gpu::interconnect;
 using N=Shader::TextureBufferType;
 PipelineCacheFileHeader header; assert(header.IsValid());
 header.version=5;assert(!header.IsValid());header.version=6;assert(!header.IsValid());
 const u32 key=0x12345678;
 const std::string path=std::string(argv[1])+"/roundtrip.pche";
 for(auto n:{N::Float,N::Uint,N::Sint}) {
  PipelineStateBundle original;
  original.Reset(key);original.AddTextureType(42,Shader::TextureType::Buffer);
  original.AddTexturePixelFormatInteger(42,n!=N::Float);original.AddTextureBufferType(42,n);
  original.AddConstantBufferValue(4,2,0x1c8,42);
  original.AddTextureCompareFunction(42,Shader::CompareFunction::Less);
  {std::ofstream f(path,std::ios::binary);original.Serialise(f);assert(f.good());}
  PipelineStateBundle loaded;
  {std::ifstream f(path,std::ios::binary);assert(loaded.Deserialise(f));assert(!loaded.Deserialise(f));}
  assert(loaded.GetKey<u32>()==key);
  assert(loaded.LookupTextureType(42)==Shader::TextureType::Buffer);
  assert(loaded.LookupTextureBufferType(42)==n);
  assert(loaded.LookupTexturePixelFormatInteger(42)==(n!=N::Float));
  assert(loaded.LookupConstantBufferValue(4,2,0x1c8)==42);
  assert(loaded.LookupTextureCompareFunction(42)==Shader::CompareFunction::Less);
  loaded.Reset(key);
  bool missing=false;try {loaded.LookupTextureBufferType(42);}catch(const std::runtime_error&){missing=true;}
  assert(missing);
 }
 std::puts("PASS production PCHE v7 payload: Float/UINT/SINT round-trips, existing tables preserved, Reset clears numeric metadata");
}
'''
    source=out/'test.cpp';source.write_text(facade+header+'\n'+cpp+'\n'+cache_header+'\n'+main)
    command=[os.environ.get('CXX','c++'),'-std=c++20','-O1','-g','-include','cstring','-include','cstddef','-DFMT_HEADER_ONLY','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'app/libraries/shader-compiler/include'),'-I'+str(root/'app/libraries/fmt/include'),str(source),os.environ.get('XXHASH_LIBRARY','-l:'+str(ctypes.util.find_library('xxhash') or 'libxxhash.so.0')),'-o',str(out/'test')]
    subprocess.run(command,check=True)
    subprocess.run([out/'test',out],check=True)
