"""Exact production segment table and map/lookup bodies; host block-vector owner."""
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[2]
def function(source, signature, defaults=False):
    start=source.index(signature)
    brace=source.index(') {',start)+2 if defaults else source.index('{',start)
    end,depth=brace+1,1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
def write_fixture(directory, namespace='segment_test', ref=None):
    def source(path):
        return subprocess.check_output(['git','show',ref+':'+path],cwd=ROOT,text=True) if ref else (ROOT/path).read_text()
    header=source('app/src/main/cpp/skyline/common/address_space.h')
    implementation=source('app/src/main/cpp/skyline/common/address_space.inc')
    table=source('app/src/main/cpp/skyline/common/segment_table.h')
    fixture=(ROOT/'tests/gpu_gmmu_identity/tests.cpp').read_text()
    fixture=fixture[fixture.index('using skyline::MappingIdentity;'):fixture.index('int main()')]
    fixture=fixture.replace('template<class...Args> std::runtime_error exception(const char *message,Args&&...) { return std::runtime_error(message); }', 'struct exception:std::runtime_error {template<class...Args> exception(const char *message,Args&&...):std::runtime_error(message){}};')
    fixture=fixture.replace('template<class T> struct span : std::span<T>', 'template<class T,size_t Extent=std::dynamic_extent> struct span : std::span<T,Extent>')
    fixture=fixture.replace('using std::span<T>::span;', 'using std::span<T,Extent>::span;')
    fixture=fixture.replace('span(std::span<T> other) : std::span<T>(other)', 'span(std::span<T,Extent> other) : std::span<T,Extent>(other)')
    fixture=fixture.replace('using TranslatedAddressRange =', function(table,'template<typename SegmentType, size_t Size')+';\nusing TranslatedAddressRange =')
    start=fixture.index('    struct Table {');end=fixture.index('    std::shared_mutex blockMutex;',start)
    fixture=fixture[:start]+'''    struct Entry {size_t virt;u8 *phys;size_t extent;Extra extraInfo;};
    SegmentTable<Entry,(1ULL<<36),12,17> blockSegmentTable;
'''+fixture[end:]
    fixture=fixture.replace('size_t vaLimit{1ULL<<40}', 'size_t vaLimit{1ULL<<36}')
    methods='\n'.join(function(header,s,True) for s in ('void Map(','void Unmap('))
    if 'ChangedMappingRangesLocked' in header:
        methods=function(header,'bool SameMappingLocked(')+'\n'+function(header,'std::vector<std::pair<VaType, VaType>> ChangedMappingRangesLocked(')+'\n'+methods
    (directory/'segment_identity.inc').write_text(methods)
    (directory/'segment_translation.inc').write_text(function(header,'std::pair<span<u8>, size_t> LookupBlockLocked(',True)+'\n'+function(header,'TranslatedAddressRange TranslateRange(',True)+'\n'+function(header,'std::pair<span<u8>, VaType> LookupBlock(',True))
    (directory/'segment_map.inc').write_text(function(implementation,'MAP_MEMBER(void)::MapLocked(')+'\n'+function(implementation,'MAP_MEMBER(void)::UnmapLocked('))
    (directory/'segment_slow.inc').write_text(function(implementation,'MM_MEMBER(TranslatedAddressRange)::TranslateRangeImpl('))
    for name in ('identity','translation','map','slow'):
        fixture=fixture.replace('"'+name+'.inc"','"segment_'+name+'.inc"')
    (directory/'segment_fixture.inc').write_text('namespace '+namespace+' {\n'+fixture+'\n}')
