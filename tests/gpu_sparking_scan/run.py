#!/usr/bin/env python3
import os,pathlib,subprocess,tempfile,shlex
root=pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    def function(text,signature):
        start=text.index(signature);end=text.index('{',start)+1;depth=1
        while depth:
            depth+=(text[end]=='{')-(text[end]=='}');end+=1
        return text[start:end]
    executor=(root/'app/src/main/cpp/skyline/gpu/interconnect/command_executor.cpp').read_text()
    signatures=['static bool IsSparkingColorImage(', 'static bool IsSparkingImage(',
                'std::optional<u32> CommandExecutor::BeginDiagnosticSparkingDraw(',
                'void CommandExecutor::EndDiagnosticSparkingDraw(', 'void CommandExecutor::QueueSparkingScanImage(']
    (pathlib.Path(tmp)/'executor.inc').write_text('\n'.join(function(executor,s) for s in signatures))
    maxwell=(root/'app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/maxwell_3d.cpp').read_text()
    (pathlib.Path(tmp)/'maxwell.inc').write_text(function(maxwell,'std::optional<u32> Maxwell3D::BeginSparkingHdrDraw('))
    exe=pathlib.Path(tmp)/'tests'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'app/src/main/cpp/skyline'),str(root/'tests/gpu_sparking_scan/tests.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','SPARKING_TEST_ROOT':tmp})
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS','-DFMT_HEADER_ONLY','-I'+str(root/'app/libraries/fmt/include'),'-I'+str(root/'app/libraries/vkhpp'),'-I'+str(root/'app/libraries/vkhpp/Vulkan-Headers/include'),*shlex.split(os.environ.get('CXXFLAGS','')),'-I'+str(root/'app/src/main/cpp/skyline'),'-I'+tmp,str(root/'tests/gpu_sparking_scan/executor.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','SPARKING_TEST_ROOT':tmp})
print('PASS: scan opt-in, single target, ordinal/byte limits, reset and HDR decoding')
