from pathlib import Path
import tempfile,subprocess,os
R=Path(__file__).resolve().parents[2];G=R/'app/src/main/cpp/skyline/gpu'
s=(G/'buffer.cpp').read_text();a=s.index('void Buffer::SynchronizeHost(');b=s.index('    bool Buffer::SynchronizeGuest(',a)
with tempfile.TemporaryDirectory()as td:
 d=Path(td);h=(G.parent/'common/trap_manager.h').read_text();start=h.index('    namespace trap_diagnostics {');end=h.index('{',start)+1;depth=1
 while depth:depth+=(h[end]=='{')-(h[end]=='}');end+=1
 (d/'trap-scope.inc').write_text('namespace skyline {using u8=unsigned char;'+h[start:end]+'}')
 parts=[]
 for signature in ['bool Buffer::DiagnosticSample()', 'unsigned Buffer::DiagnosticFlags()', 'void Buffer::DiagnosticCycle(']:
  start=s.index(signature);end=s.index('{',start)+1;depth=1
  while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
  parts.append(s[start:end])
 (d/'host.inc').write_text('\n'.join(parts)+s[a:b])
 start=s.index('                ++buffer->diagnosticEpoch;buffer->diagnosticCause=1;')
 end=s.index('                return true;',start)+len('                return true;')
 (d/'fast.inc').write_text('using DirtyState=Buffer::DirtyState;\nbool FastCallback(Buffer *buffer){\n'+s[start:end]+'\n}')
 exe=d/'test'
 subprocess.run(['c++','-std=c++20','-pthread','-Wall','-Wextra','-Werror','-fsanitize='+os.environ.get('SANITIZERS','undefined'),'-fno-sanitize-recover=all','-I'+str(G),'-I'+str(d),str(Path(__file__).with_name('test.cpp')),'-o',str(exe)],check=True);subprocess.run([exe],check=True)
