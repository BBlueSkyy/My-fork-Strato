#!/usr/bin/env python3
"""Run CXX-aware production-body regression scripts as ARM64 Linux under QEMU.

Requires g++-aarch64-linux-gnu and qemu-user (or qemu-user-static). Override
ARM64_CXX, ARM64_QEMU, ARM64_SYSROOT (runtime) and optional
ARM64_CXX_SYSROOT (linker) for a relocated toolchain. Uses UBSan except vptr (GCC/libstdc++ type_info recursion under QEMU);
the native suites retain ASan. This does not validate Android/Vulkan drivers.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

if os.environ.get('STRATO_ARM64_COMPILER_WRAPPER') == __file__:
    args = sys.argv[1:]
    args = [('-fsanitize=undefined' if value.startswith('-fsanitize=') else value) for value in args]
    # GCC 13 UBSan vptr instrumentation recurses in libstdc++ type_info::operator==
    # even for a standalone make_shared/shared_from_this program under QEMU.
    # Native ASan/UBSan suites still cover vptr; keep all other ARM64 UBSan checks.
    args.append('-fno-sanitize=vptr')
    compiler = os.environ['ARM64_CXX_RESOLVED']
    sysroot = os.environ.get('ARM64_CXX_SYSROOT')
    subprocess.run([compiler, *(['--sysroot=' + sysroot] if sysroot else []), *args], check=True)
    if '-o' in args and '-c' not in args:
        output = pathlib.Path(args[args.index('-o') + 1])
        header = output.read_bytes()[:20]
        if header[:4] != b'\x7fELF' or int.from_bytes(header[18:20], 'little') != 183:
            raise RuntimeError('Cross compiler did not produce ARM64 ELF')
        binary = output.with_name(output.name + '.arm64')
        output.rename(binary)
        command = [os.environ['ARM64_QEMU_RESOLVED'], '-L', os.environ.get('ARM64_SYSROOT', '/usr/aarch64-linux-gnu'), str(binary)]
        output.write_text('#!' + sys.executable + '\nimport os,sys\nos.execv(' + repr(command[0]) + ', ' + repr(command) + '+sys.argv[1:])\n')
        output.chmod(0o755)
    sys.exit(0)

compiler = shutil.which(os.environ.get('ARM64_CXX', 'aarch64-linux-gnu-g++'))
qemu = shutil.which(os.environ.get('ARM64_QEMU', 'qemu-aarch64')) or shutil.which('qemu-aarch64-static')
if not compiler or not qemu:
    raise SystemExit('Install g++-aarch64-linux-gnu and qemu-user, or set ARM64_CXX/ARM64_QEMU')
scripts = sys.argv[1:] or ['tests/gpu_constant_buffers/run.py', 'tests/gmmu/run.py', 'tests/gpu_disjoint_buffers/run.py']
with tempfile.TemporaryDirectory(prefix='strato-arm64-') as directory:
    wrapper = pathlib.Path(directory) / 'cxx'
    source = pathlib.Path(__file__).resolve()
    wrapper.write_text('#!' + sys.executable + '\nimport runpy\nrunpy.run_path(' + repr(str(source)) + ', run_name="__main__")\n')
    wrapper.chmod(0o755)
    env = {**os.environ, 'CXX': str(wrapper), 'STRATO_ARM64_COMPILER_WRAPPER': str(source),
           'ARM64_CXX_RESOLVED': compiler, 'ARM64_QEMU_RESOLVED': qemu}
    print('ARM64 Linux under QEMU: UBSan enabled (except vptr); Android/driver testing remains separate', flush=True)
    for script in scripts:
        subprocess.run([sys.executable, script], cwd=source.parents[1], env=env, check=True)
