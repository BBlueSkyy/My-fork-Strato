#!/usr/bin/env python3
from pathlib import Path
import os,subprocess,tempfile
here=Path(__file__).resolve().parent
cpp=here.parents[1]/'app/src/main/cpp/skyline'
with tempfile.TemporaryDirectory(prefix='strato-region-map-') as td:
    td=Path(td)
    text=(cpp/'common/interval_map.h').read_text().replace('#include "utils.h"','#include "host_types.h"').replace('#include "span.h"','')
    (td/'interval_map.h').write_text(text)
    (td/'trap_manager.h').write_text((cpp/'common/trap_manager.h').read_text())
    source=(cpp/'common/trap_manager.cpp').read_text()
    (td/'trap.inc').write_text(source[source.index('namespace skyline {'):])
    command=[os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-Wno-invalid-offsetof',
             '-Wno-unused-parameter','-Wno-return-type','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(here),'-I'+str(td),str(here/'range_map_test.cpp'),'-o',str(td/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(td/'test')],check=True,timeout=30)
