#!/usr/bin/env python3
"""Replay user-provided PCHE v5 fragments; no game binaries are distributed."""
import argparse, ctypes, ctypes.util, hashlib, json, pathlib, struct, subprocess
from prove import prove

def read_cache(path):
    data=path.read_bytes(); lib=ctypes.CDLL(ctypes.util.find_library('xxhash'));lib.XXH64.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulonglong];lib.XXH64.restype=ctypes.c_ulonglong
    if len(data)<12 or data[:4]!=b'PCHE':raise ValueError('invalid PCHE magic')
    ver,count=struct.unpack_from('<2I',data,4)
    if ver!=5:raise ValueError(f'unsupported cache version {ver}')
    pos=12;out=[]
    def take(blob,off,fmt):
        size=struct.calcsize(fmt)
        if off+size>len(blob):raise ValueError('truncated cache')
        return struct.unpack_from(fmt,blob,off),off+size
    for n in range(count):
        (recorded,size),pos=take(data,pos,'<QI')
        if size>1<<20 or pos+size>len(data):raise ValueError('invalid bundle size')
        b=data[pos:pos+size];pos+=size
        if lib.XXH64(b,len(b),0)!=recorded:raise ValueError(f'XXH64 mismatch at bundle {n}')
        (ks,cb,ty,fmt,cp,st),off=take(b,0,'<6I')
        if ks>5000 or max(cb,ty,fmt,cp)>5000 or st>16:raise ValueError('invalid bundle count')
        key=b[off:off+ks];off+=ks;tables={}
        for tag,c,form in [('cbuf',cb,'<4I'),('type',ty,'<2I'),('integer',fmt,'<2I'),('compare',cp,'<2I')]:
            tables[tag]=[]
            for _ in range(c):v,off=take(b,off,form);tables[tag].append(v)
        stages=[]
        for _ in range(st):
            (base,length),off=take(b,off,'<2I')
            if off+length>len(b):raise ValueError('truncated stage')
            stages.append((base,b[off:off+length]));off+=length
        if off!=len(b):raise ValueError('unparsed bundle bytes')
        out.append(dict(index=n,key=key,tables=tables,stages=stages))
    if pos!=len(data):raise ValueError('unparsed cache tail')
    return out

def state(key):
    # Android/AArch64 Clang layout of the production PackedPipelineState.
    # Prefix layout: 6 u64 hashes, 2x2B stencil records, 8B flags at52,
    # patch60, alpha64, point68, packed attributes72, formats200,
    # packed CtSelect208, post-VTG mask212. Full v5 key size1496.
    if len(key)!=1496:raise ValueError('unexpected packed graphics key size')
    return dict(texture_cbuf=key[57]&31,viewport=(key[58]>>6)&1,
                early=(key[57]>>5)&1,flip=(key[54]>>5)&1,
                alpha=(key[58]>>3)&1,alpha_func=key[58]&7,
                alpha_ref=struct.unpack_from('<f',key,64)[0],
                skip=list(struct.unpack_from('<8I',key,212)))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cache',type=pathlib.Path,required=True);p.add_argument('--binary',type=pathlib.Path,required=True);p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--spirv-tools',type=pathlib.Path,required=True);p.add_argument('--hash',action='append',default=[]);a=p.parse_args()
    records=read_cache(a.cache);a.out.mkdir(parents=True,exist_ok=True)
    targets=a.hash or ['E7582A61F9097487','BC3F7489079E8ABE'];summary={'cache_sha256':hashlib.sha256(a.cache.read_bytes()).hexdigest(),'verified_bundles':len(records),'shaders':[]}
    for name in targets:
        h=int(name,16);hits=[r for r in records if len(r['key'])>=48 and struct.unpack_from('<Q',r['key'],40)[0]==h]
        if len(hits)!=1:raise ValueError(f'{name}: expected exactly one fragment key, found {len(hits)}')
        r=hits[0];base,code=r['stages'][5];s=state(r['key']);prefix=a.out/name.upper();prefix.with_suffix('.maxwell.bin').write_bytes(code)
        lines=[' '.join(map(str,[base,s['texture_cbuf'],s['viewport'],s['early'],s['flip'],s['alpha'],s['alpha_func'],s['alpha_ref']])),' '.join(map(str,s['skip']))]
        for tag,rows in r['tables'].items():
            for row in rows:
                if tag=='cbuf':
                    stage,index,offset,value=row
                    if stage!=4:continue
                    row=(index,offset,value)
                lines.append(tag+' '+' '.join(map(str,row)))
        meta=prefix.with_suffix('.env');meta.write_text('\n'.join(lines)+'\n')
        run=subprocess.run([str(a.binary.resolve()),str(prefix.with_suffix('.maxwell.bin')),str(meta),str(prefix)],capture_output=True,text=True);prefix.with_suffix('.compile.log').write_text(run.stdout+run.stderr);run.check_returncode()
        for tool,extra in [('spirv-val',['--target-env','vulkan1.1spv1.4','--uniform-buffer-standard-layout']),('spirv-dis',['--raw-id','-o',str(prefix.with_suffix('.spvasm'))])]:
            result=subprocess.run([str(a.spirv_tools/tool),*extra,str(prefix.with_suffix('.spv'))],capture_output=True,text=True);prefix.with_suffix('.'+tool+'.log').write_text(result.stdout+result.stderr);result.check_returncode()
        summary['shaders'].append(dict(hash=name.upper(),pipeline_entry=r['index'],base=base,bytes=len(code),sha256=hashlib.sha256(code).hexdigest(),state=s,spv_sha256=hashlib.sha256(prefix.with_suffix('.spv').read_bytes()).hexdigest(),predication_proof=prove(prefix.with_suffix('.spvasm'))))
        print(name,run.stdout.strip(),'spirv-val PASS',flush=True)
    (a.out/'replay-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
if __name__=='__main__':main()
