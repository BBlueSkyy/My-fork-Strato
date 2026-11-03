#!/usr/bin/env python3
"""Run production selector/cache/bind and macro bodies with host memory facades."""
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT/'app/src/main/cpp/skyline/gpu/interconnect'
replay_ref=os.environ.get('CBUF_CASTLE_REPLAY_REF')
def read_source(path):
    return subprocess.check_output(['git','show',replay_ref+':'+str(path.relative_to(ROOT))],cwd=ROOT,text=True) if replay_ref else path.read_text()
def function(source, signature, defaults=False):
    start=source.index(signature)
    if '::ConstantBuffer' in signature:
        return source[start:source.index('\n',start)]
    brace=source.index(') {',start)+2 if defaults else source.index('{',start)
    end,depth=brace+1,1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
with tempfile.TemporaryDirectory(prefix='strato-cbuf-selector-') as tmp:
    tmp=Path(tmp)
    common=read_source((GPU/'common/common.h'))
    (tmp/'cache_class.inc').write_text(function(common,'class CachedMappedBufferView')+';')
    source=read_source((GPU/'common/common.cpp'))
    (tmp/'cache.inc').write_text(function(source,'void CachedMappedBufferView::Update(')+ '\n'+function(source,'void CachedMappedBufferView::PurgeCaches('))
    header=read_source((GPU/'maxwell_3d/constant_buffers.h'))
    (tmp/'selector_class.inc').write_text(function(header,'class ConstantBufferSelectorState')+';')
    (tmp/'buffers_class.inc').write_text(function(header,'class ConstantBuffers {')+';')
    source=read_source((GPU/'maxwell_3d/constant_buffers.cpp'))
    signatures=['void ConstantBufferSelectorState::EngineRegisters::DirtyBind(', 'ConstantBufferSelectorState::ConstantBufferSelectorState(', 'void ConstantBufferSelectorState::Flush(', 'bool ConstantBufferSelectorState::Refresh(', 'void ConstantBufferSelectorState::PurgeCaches(', 'ConstantBuffers::ConstantBuffers(', 'void ConstantBuffers::Bind(', 'void ConstantBuffers::MarkAllDirty(', 'void ConstantBuffers::DisableQuickBind(']
    if 'void ConstantBufferSelectorState::DiagnoseBindFailure(' in source:
        signatures+=['void ConstantBufferSelectorState::RecordDiagnostic(', 'void ConstantBufferSelectorState::DiagnoseBindFailure(']
    (tmp/'selector.inc').write_text('\n'.join(function(source,s) for s in signatures))
    dirty=(ROOT/'app/src/main/cpp/skyline/common/dirty_tracking.h').read_text()
    (tmp/'dirty.inc').write_text(function(dirty,'template<typename T> requires (std::is_base_of_v<ManualDirty, T>)')+';')
    macro=read_source((ROOT/'app/src/main/cpp/skyline/soc/gm20b/macro/macro_interpreter.h'))
    (tmp/'macro_class.inc').write_text(function(macro,'class MacroInterpreter')+';')
    macro=read_source((ROOT/'app/src/main/cpp/skyline/soc/gm20b/macro/macro_interpreter.cpp'))
    (tmp/'macro.inc').write_text(macro[macro.index('namespace skyline'):])
    address=(ROOT/'app/src/main/cpp/skyline/common/address_space.h').read_text()
    address_source=(ROOT/'app/src/main/cpp/skyline/common/address_space.inc').read_text()
    (tmp/'identity.inc').write_text('\n'.join(function(address,s,d) for s,d in [('bool SameMappingLocked(',False),('std::vector<std::pair<VaType, VaType>> ChangedMappingRangesLocked(',False),('TrackedTranslation TranslateRangeTracked(',True),('void Map(',True),('void Unmap(',True)]))
    (tmp/'translation.inc').write_text(function(address,'std::pair<span<u8>, size_t> LookupBlockLocked(',True)+'\n'+function(address,'TranslatedAddressRange TranslateRange(',True)+'\n'+function(address,'std::pair<span<u8>, VaType> LookupBlock(',True))
    (tmp/'map.inc').write_text(function(address_source,'MAP_MEMBER(void)::MapLocked(')+'\n'+function(address_source,'MAP_MEMBER(void)::UnmapLocked('))
    (tmp/'slow.inc').write_text(function(address_source,'MM_MEMBER(TranslatedAddressRange)::TranslateRangeImpl('))
    fixture=(ROOT/'tests/gpu_gmmu_identity/tests.cpp').read_text()
    fixture=fixture[fixture.index('using skyline::MappingIdentity;'):fixture.index('int main()')]
    fixture=fixture.replace('uint64_t MappingRevision() const', 'skyline::MappingIdentity::Description InspectMappingIdentity(size_t address) const { return mappingIdentity.Inspect(address); }\n    uint64_t MappingRevision() const')
    (tmp/'gmmu.inc').write_text('namespace translation_test {\n'+fixture+'\n}')
    compiler=os.environ.get('CXX','clang++')
    if not shutil.which(compiler):
        raise SystemExit('Set CXX to a Clang C++20 compiler (Android production macro always_inline bodies require Clang).')
    command=[compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(tmp),'-I'+str(ROOT/'app/src/main/cpp/skyline'),str(Path(__file__).with_name('tests.cpp')),'-o',str(tmp/'tests')]
    subprocess.run(command,check=True)
    env=dict(os.environ,ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS','')+':detect_leaks=0')
    subprocess.run([str(tmp/'tests')],check=True,env=env,timeout=30)
