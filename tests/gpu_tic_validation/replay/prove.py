#!/usr/bin/env python3
"""Focused structural/control-flow proof over the generated SPIR-V disassembly.

This is not an execution engine. It evaluates only the exact uniform comparison
needed to select CFG edges, and fails closed on any additional branch condition.
"""
import argparse, json, math, pathlib, re

def prove(path):
    inst={};names={};blocks={};block=None;line_of={};constants={};array_stride={}
    for ln,line in enumerate(path.read_text().splitlines(),1):
        t=line.split(';',1)[0].split()
        if not t:continue
        rid=None
        if len(t)>2 and t[1]=='=':rid=t[0];op=t[2];args=t[3:];inst[rid]=(op,args);line_of[rid]=ln
        else:op=t[0];args=t[1:]
        if op=='OpName':names[args[0]]=args[1].strip('"')
        if op=='OpDecorate' and args[1]=='ArrayStride':array_stride[args[0]]=int(args[2])
        if op=='OpConstant':constants[rid]=float(args[1])
        if op=='OpLabel':block=rid;blocks[block]=[]
        if block:blocks[block].append((rid,op,args,ln))
        if op=='OpFunctionEnd':block=None
    def unique(items,desc):
        if len(items)!=1:raise AssertionError(f'expected exactly one {desc}, found {items}')
        return items[0]
    cb=unique([k for k,v in names.items() if v=='c3_f_4'],'runtime CBUF3 variable')
    tex=unique([k for k,v in names.items() if v=='fs_tex2_30'],'CBUF2[0x30] texture variable')
    # Resolve the actual uniform scalar offset from its type/array stride and
    # indices, rather than interpreting an IR/Maxwell annotation as SPIR-V proof.
    ptrtype=inst[cb][1][0];structtype=inst[ptrtype][1][1];arraytype=inst[structtype][1][0]
    stride=array_stride[arraytype];assert stride==4
    accesses=[k for k,(op,a) in inst.items() if op=='OpAccessChain' and a[1]==cb and len(a)==4 and constants[a[2]]==0 and constants[a[3]]*stride==4]
    loads=[k for k,(op,a) in inst.items() if op=='OpLoad' and a[1] in accesses]
    pred=unique([k for k,(op,a) in inst.items() if op=='OpFUnordNotEqual' and a[1] in constants and constants[a[1]]==0 and a[2] in loads],'P0 uniform comparison')
    texloads=[k for k,(op,a) in inst.items() if op=='OpLoad' and a[1]==tex]
    samples=[(b,r,ln) for b,body in blocks.items() for r,op,a,ln in body if op=='OpImageSampleImplicitLod' and a[1] in texloads]
    target,sample,line=unique(samples,'CBUF2[0x30] sample')
    all_samples=[(b,r)for b,body in blocks.items()for r,op,a,ln in body if op=='OpImageSampleImplicitLod'];assert len(all_samples)==6
    entry=next(iter(blocks))
    def reachable(p0):
        seen=set();todo=[entry]
        while todo:
            b=todo.pop()
            if b in seen:continue
            seen.add(b)
            for _,op,a,_ in blocks[b]:
                if op=='OpBranch':todo.append(a[0])
                if op=='OpBranchConditional':
                    assert a[0]==pred, f'unmodeled branch predicate {a[0]}'
                    todo.append(a[1] if p0 else a[2])
        return seen
    zero=reachable(False);nonzero=reachable(True)
    assert target not in zero and target in nonzero
    for seen in [zero,nonzero]:
        assert any(op=='OpReturn' for b in seen for _,op,_,_ in blocks[b]), 'selected path does not reach a return'
    branches=[(b,a,ln)for b,body in blocks.items()for r,op,a,ln in body if op=='OpBranchConditional' and a[1]==target]
    parent,branch,branch_line=unique(branches,'true edge into the sample block');assert branch[0]==pred
    # Every reachable predecessor must be the comparison's true edge.
    incoming=[(b,op,a)for b,body in blocks.items()for r,op,a,ln in body if (op=='OpBranch' and a[0]==target)or(op=='OpBranchConditional' and target in a[1:3])]
    assert len(incoming)==1 and incoming[0][0]==parent
    result={'module':path.name,'predicate':pred,'uniform_variable':cb,'uniform_byte_offset':4,'predicate_opcode':'OpFUnordNotEqual','texture_variable':tex,'sample':sample,'sample_block':target,'branch_block':parent,'branch_true':branch[1],'branch_false':branch[2],'sample_line':line,'branch_line':branch_line,'samples_retained':len(all_samples),'runtime_cases':[]}
    for label,v in [('positive_zero',0.0),('negative_zero',-0.0),('one',1.0),('negative_one',-1.0),('NaN',float('nan'))]:
        p0=math.isnan(v) or v!=0.0;seen=reachable(p0);count=sum(b in seen for b,_ in all_samples)
        result['runtime_cases'].append({'CBUF3_4':label,'P0':p0,'sample_2_30_reachable':target in seen,'reachable_samples':count})
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('disassembly',nargs='+',type=pathlib.Path);p.add_argument('--out',type=pathlib.Path);a=p.parse_args();r=[prove(f) for f in a.disassembly];s=json.dumps(r,indent=2)+'\n'
    if a.out:a.out.write_text(s)
    print(s,end='')
if __name__=='__main__':main()
