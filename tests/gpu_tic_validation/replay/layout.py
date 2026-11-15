#!/usr/bin/env python3
"""Check production PCHE graphics-key ABI offsets with Android/AArch64 Clang.

Uses source field declarations verbatim, substituting only equivalent primitive
ABI types and fixed-size arrays, so the check does not need Android/Vulkan SDKs.
"""
import argparse, pathlib, re, subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--clang',default='clang++');a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[3]
s=(root/'app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/packed_pipeline_state.h').read_text();s=s[s.index('    struct PackedPipelineState {'):s.index('        void SetColorRenderTargetFormat')]
start=s.index('            vk::VertexInputRate GetInputRate() const {');end=s.index('\n            }',start)+len('\n            }');s=s[:start]+s[end:]
for name,value in [('PipelineCount',6),('VertexAttributeCount',32),('ColorTargetCount',8),('VertexStreamCount',16)]:s=s.replace('engine::'+name,str(value))
for name,typ in [('DrawTopology','E16'),('TessellationParameters::DomainType','E8'),('TessellationParameters::Spacing','E8'),('TessellationParameters::OutputPrimitives','E8'),('ProvokingVertex::Value','E8'),('VertexAttribute','VA'),('CtSelect','CT')]:s=s.replace('engine::'+name,typ)
s=s.replace('std::array','Arr')
prefix='using u8=unsigned char;using u16=unsigned short;using u32=unsigned;using u64=unsigned long long;using VkCullModeFlags=u32;using VkColorComponentFlags=u32;template<class T,unsigned N>struct Arr{T a[N];};enum class E16:u16{};enum class E8:u8{};struct __attribute__((packed)) VA{u32 x;};struct __attribute__((packed)) CT{u32 x;};\n'
a.out.mkdir(parents=True,exist_ok=True);source=a.out/'packed-key-layout.cpp';source.write_text(prefix+s+'};\nPackedPipelineState force;\n')
r=subprocess.run([a.clang,'-target','aarch64-linux-android','-std=c++20','-Xclang','-fdump-record-layouts-complete','-fsyntax-only',str(source)],capture_output=True,text=True);r.check_returncode();(a.out/'packed-key-layout.txt').write_text(r.stdout)
record=r.stdout.split('struct PackedPipelineState\n')[-1]
for pattern in [r'57:0-4.*bindlessTextureConstantBufferSlotSelect',r'57:5-5.*apiMandatedEarlyZ',r'58:3-3.*alphaTestEnable',r'58:6-6.*viewportTransformEnable',r'54:5-5.*flipYEnable',r'64 \|.*alphaRef',r'212 \|.*postVtgShaderAttributeSkipMask',r'\[sizeof=1496,']:
    assert re.search(pattern,record),f'unexpected production ABI: {pattern}'
print('PASS: Android/AArch64 production key size1496; replay field offsets verified')
