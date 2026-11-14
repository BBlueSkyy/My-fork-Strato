#!/usr/bin/env python3
"""Real Vulkan nullDescriptor sample/query/storage execution. Requires validation.
This verifies host behavior only, never Maxwell's reserved-TIC semantics.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
HERE = Path(__file__).resolve().parent
assembler = shutil.which('spirv-as')
validator = shutil.which('spirv-val')
if not assembler or not validator:
    raise SystemExit('spirv-as and spirv-val are required')
# Dim, arrayed, coordinate components, query-size components
cases = [('1D', 0, 1, 1), ('1D', 1, 2, 2), ('2D', 0, 2, 2), ('2D', 1, 3, 3),
         ('3D', 0, 3, 3), ('Cube', 0, 3, 2), ('Cube', 1, 4, 3)]

def module(dim, arrayed, coordinates, queries):
    storage = dim == 'storage'
    header = '''OpCapability Shader
OpCapability ImageQuery
'''
    if dim == '1D': header += 'OpCapability Sampled1D\n'
    if dim == 'Cube' and arrayed: header += 'OpCapability SampledCubeArray\n'
    header += '''OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %resource DescriptorSet 0
OpDecorate %resource Binding 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 1
OpDecorate %Result Block
OpMemberDecorate %Result 0 Offset 0
OpMemberDecorate %Result 1 Offset 16
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%u0 = OpConstant %uint 0
%u1 = OpConstant %uint 1
%f0 = OpConstant %float 0
%v2uint = OpTypeVector %uint 2
%v3uint = OpTypeVector %uint 3
%v4uint = OpTypeVector %uint 4
%v2float = OpTypeVector %float 2
%v3float = OpTypeVector %float 3
%v4float = OpTypeVector %float 4
%coord2 = OpConstantComposite %v2float %f0 %f0
%coord3 = OpConstantComposite %v3float %f0 %f0 %f0
%coord4 = OpConstantComposite %v4float %f0 %f0 %f0 %f0
%icoord = OpConstantComposite %v2uint %u0 %u0
%ones = OpConstantComposite %v4uint %u1 %u1 %u1 %u1
%Result = OpTypeStruct %v4uint %v4uint
%pResult = OpTypePointer StorageBuffer %Result
%pvec = OpTypePointer StorageBuffer %v4uint
%output = OpVariable %pResult StorageBuffer
'''
    if storage:
        header += '%Image = OpTypeImage %uint 2D 0 0 0 2 R32ui\n%pImage = OpTypePointer UniformConstant %Image\n%resource = OpVariable %pImage UniformConstant\n'
    else:
        header += f'%Image = OpTypeImage %float {dim} 0 {arrayed} 0 1 Unknown\n%Sampled = OpTypeSampledImage %Image\n%pSampled = OpTypePointer UniformConstant %Sampled\n%resource = OpVariable %pSampled UniformConstant\n'
    header += '%main = OpFunction %void None %fn\n%entry = OpLabel\n'
    if storage:
        header += '%image = OpLoad %Image %resource\nOpImageWrite %image %icoord %ones\n%bits = OpImageRead %v4uint %image %icoord\n%size = OpImageQuerySize %v2uint %image\n'
    else:
        coord = '%f0' if coordinates == 1 else '%coord' + str(coordinates)
        header += f'%sampled = OpLoad %Sampled %resource\n%value = OpImageSampleExplicitLod %v4float %sampled {coord} Lod %f0\n%bits = OpBitcast %v4uint %value\n%image = OpImage %Image %sampled\n'
        querytype = '%uint' if queries == 1 else '%v' + str(queries) + 'uint'
        header += f'%size = OpImageQuerySizeLod {querytype} %image %u0\n'
    if queries == 1:
        components = ['%size']
    else:
        components = []
        for n in range(queries):
            header += f'%q{n} = OpCompositeExtract %uint %size {n}\n'
            components.append('%q' + str(n))
    header += '%sizes = OpCompositeConstruct %v4uint ' + ' '.join(components + ['%u0'] * (4 - queries)) + '\n'
    return header + '''%sampleOut = OpAccessChain %pvec %output %u0
%sizeOut = OpAccessChain %pvec %output %u1
OpStore %sampleOut %bits
OpStore %sizeOut %sizes
OpReturn
OpFunctionEnd
'''

with tempfile.TemporaryDirectory(prefix='strato-null-vulkan-') as directory:
    build = Path(directory)
    for i, case in enumerate(cases + [('storage', 0, 2, 2)]):
        source = build / f'{i}.spvasm'
        source.write_text(module(*case))
        binary = source.with_suffix('.spv')
        subprocess.run([assembler, '--target-env', 'vulkan1.1', str(source), '-o', str(binary)], check=True)
        subprocess.run([validator, '--target-env', 'vulkan1.1', str(binary)], check=True)
    executable = build / 'tests'
    include = os.environ.get('VULKAN_HEADERS_INCLUDE_DIR', '/usr/include')
    loader = os.environ.get('VULKAN_LOADER_LIBRARY', '-lvulkan')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-missing-field-initializers', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    '-I' + include, str(HERE / 'vulkan.cpp'), loader, '-o', str(executable)], check=True)
    subprocess.run([str(executable), str(build)], check=True)
