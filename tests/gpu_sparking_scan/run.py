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
    dma=(root/'app/src/main/cpp/skyline/soc/gm20b/engines/maxwell_dma.cpp').read_text()
    i2m=(root/'app/src/main/cpp/skyline/soc/gm20b/engines/inline2memory.cpp').read_text()
    hook='if (gpu::diagnostic::SparkingUploads().enabled.load(std::memory_order_relaxed))'
    (pathlib.Path(tmp)/'dma_trace.inc').write_text(function(dma,hook))
    (pathlib.Path(tmp)/'i2m_trace.inc').write_text(function(i2m,hook))
    dma_h=(root/'app/src/main/cpp/skyline/soc/gm20b/engines/maxwell_dma.h').read_text()
    i2m_h=(root/'app/src/main/cpp/skyline/soc/gm20b/engines/inline2memory.h').read_text()
    (pathlib.Path(tmp)/'dma_types.inc').write_text('\n'.join(function(dma_h,'struct '+n)+';' for n in ['LaunchDma','RemapComponents','Surface']))
    (pathlib.Path(tmp)/'i2m_type.inc').write_text(function(i2m_h,'struct RegisterState').replace('Address offsetOut;','u64 offsetOut;')+';')
    exe=pathlib.Path(tmp)/'tests'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'app/src/main/cpp/skyline'),str(root/'tests/gpu_sparking_scan/tests.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','SPARKING_TEST_ROOT':tmp})
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'app/src/main/cpp/skyline'),'-I'+tmp,str(root/'tests/gpu_sparking_scan/transfers.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS','-DFMT_HEADER_ONLY','-I'+str(root/'app/libraries/fmt/include'),'-I'+str(root/'app/libraries/vkhpp'),'-I'+str(root/'app/libraries/vkhpp/Vulkan-Headers/include'),*shlex.split(os.environ.get('CXXFLAGS','')),'-I'+str(root/'app/src/main/cpp/skyline'),'-I'+tmp,str(root/'tests/gpu_sparking_scan/executor.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','SPARKING_TEST_ROOT':tmp})
print('PASS: scan opt-in, single target, ordinal/byte limits, reset and HDR decoding')
