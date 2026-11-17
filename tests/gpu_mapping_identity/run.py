#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='strato-mapping-identity-') as temp:
    temp=Path(temp)
    header=(root/'app/src/main/cpp/skyline/common/mapping_identity.h').read_text()
    header=header.replace('#include <vector>','#include <vector>\n#include <functional>')
    header=header.replace('namespace skyline {','namespace skyline {\n    inline std::function<void()> identityValidationHook;',1)
    hook='if (!receipt->current.load(std::memory_order_acquire)) return false;'
    header=header.replace(hook,hook+'\n                if (identityValidationHook) identityValidationHook();')
    if os.environ.get('IDENTITY_VALIDITY_BASELINE'):
        header=header.replace('return receipt->current.load(std::memory_order_acquire);','return true;')
        header=header.replace('else receipt->checkedRevision.store','receipt->checkedRevision.store')
    (temp/'common').mkdir()
    (temp/'common/mapping_identity.h').write_text(header)
    executable=temp/'tests'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-pthread',
        '-DIDENTITY_RACE_PROBE','-I'+str(temp),'-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'app/src/main/cpp/skyline'),
        str(Path(__file__).with_name('tests.cpp')),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True,timeout=30)
