from pathlib import Path
import tempfile,subprocess
R=Path(__file__).resolve().parents[2];G=R/'app/src/main/cpp/skyline/gpu'
s=(G/'buffer.cpp').read_text();a=s.index('void Buffer::SynchronizeHost(');b=s.index('    bool Buffer::SynchronizeGuest(',a)
with tempfile.TemporaryDirectory()as td:
 d=Path(td);(d/'host.inc').write_text(s[a:b]);exe=d/'test'
 subprocess.run(['c++','-std=c++20','-pthread','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(G),'-I'+str(d),str(Path(__file__).with_name('test.cpp')),'-o',str(exe)],check=True);subprocess.run([exe],check=True)
