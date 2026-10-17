# SPDX-License-Identifier: MPL-2.0
"""Optional private binary replay. No game binaries are included or downloaded.

Requires lz4 and unicorn. Pass the matching main and sdk NSOs using --main and
--sdk. This executes the original input polling function and SDK readers in
Unicorn with controlled mappings/stack state, not a complete game or Strato NCE.
"""
import argparse
import hashlib
import json
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import lz4.block
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_INVALID
from unicorn.arm64_const import *

MAIN=0x10000000;SDK=0x20000000;OBJ=0x30000000;STACK=0x31000000;HID=0x32000000;STOP=0x33000000

def load_nso(path, expected_sha, memory_size):
    raw=Path(path).read_bytes()
    if hashlib.sha256(raw).hexdigest()!=expected_sha:
        raise ValueError("Unexpected NSO revision; offsets in this replay do not apply")
    flags=struct.unpack_from('<I',raw,12)[0]
    data=bytearray(memory_size)
    for i,offset in enumerate([0x10,0x20,0x30]):
        file_offset,address,size=struct.unpack_from('<III',raw,offset)
        compressed_size=struct.unpack_from('<I',raw,0x60+i*4)[0]
        payload=lz4.block.decompress(raw[file_offset:file_offset+compressed_size],uncompressed_size=size) if flags&(1<<i) else raw[file_offset:file_offset+size]
        assert hashlib.sha256(payload).digest()==raw[0xa0+i*32:0xc0+i*32]
        data[address:address+size]=payload
    d=dynamic(data)
    _,count=struct.unpack_from('<II',data,d[4])
    syms=[]
    for i in range(count):
        name,info,other,section,value,size=struct.unpack_from('<IBBHQQ',data,d[6]+i*24)
        end=data.index(0,d[5]+name)
        syms.append(dict(name=data[d[5]+name:end].decode(),section=section,value=value))
    return bytes(data),syms

def dynamic(b):
 m=struct.unpack_from('<I',b,4)[0];a=m+struct.unpack_from('<i',b,m+4)[0];d={}
 while True:
  t,v=struct.unpack_from('<QQ',b,a);a+=16
  if not t:return d
  d[t]=v

def test(kind='FullKey',debug_samples=0,poison=(16384,-8192),npad_axes=(0,0,0,0), production=False):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 for a,n in [(MAIN,0x5000000),(SDK,0x1200000),(OBJ,0x10000),(STACK,0x10000),(HID,0x40000),(STOP,0x1000)]:u.mem_map(a,n)
 u.mem_write(MAIN,M);u.mem_write(SDK,S)
 def put(a,fmt,*v):u.mem_write(a,struct.pack(fmt,*v))
 def relocate(b,base,syms,resolve):
  d=dynamic(b)
  for a,n in [(d[7],d[8]),(d[23],d[2])]:
   for p in range(a,a+n,24):
    off,inf,add=struct.unpack_from('<QQq',b,p);typ=inf&0xffffffff;idx=inf>>32
    if typ==1027:put(base+off,'<Q',base+add)
    elif typ in (1025,1026):
     sym=syms[idx];val=base+sym['value'] if sym['section'] else resolve(sym['name'])
     if val:put(base+off,'<Q',val+add)
 relocate(S,SDK,SS,lambda n:0)
 relocate(M,MAIN,MS,lambda n:SDK+SM[n]['value'] if n in SM and SM[n]['section'] else 0)
 u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
 # Model the already initialized SDK's shared-memory mappings, not its IPC startup.
 u.mem_write(SDK+0xa27a60,b'\1');u.mem_write(SDK+0xa27b90,b'\1');u.mem_write(SDK+0xa26168,b'\1');put(SDK+0xa26160,'<Q',HID)
 off,style,ident={'FullKey':(0x28,1,0),'JoyDual':(0x6c8,4,0),'Handheld':(0x378,2,32)}[kind]
 for i in range(10):
  ptr=HID+0x9a00+i*0x5000
  put(SDK+0xa27ac0+8*i,'<Q',ptr)
  put(ptr,'<I',style if i==(8 if ident==32 else 0) else 0)
  for ring in [0x28,0x378,0x6c8,0xa18,0xd68,0x10b8,0x1408]:
   put(ptr+ring,'<QQQQ',123456,17,40%17,16)
   for seq in range(24,41):put(ptr+ring+0x20+(seq%17)*0x30,'<QQQiiiiII',seq*2,seq,0,*npad_axes,3,0)
 put(HID,'<QQQQ',0,17,40%17,debug_samples)
 for seq in range(24,41):put(HID+0x20+(seq%17)*0x28,'<QQIIiiii',seq*2,seq,0,0,0,0,0,0)
 if production:u.mem_write(HID,PRODUCTION)
 put(OBJ+0x3ac,'<I',ident);put(OBJ+0x3b0,'<i',-1)
 initialsp=STACK+0xf000;frame=initialsp-0xc0
 # Buffer reused on game's stack; disconnected DebugPad must overwrite it with neutral state.
 put(frame+0x20,'<QIIiiii',0,0,0,0,0,*poison)
 u.reg_write(UC_ARM64_REG_SP,initialsp);u.reg_write(UC_ARM64_REG_LR,STOP);u.reg_write(UC_ARM64_REG_X0,OBJ)
 trace=[];dbgreads=[];lastpc=0
 def code(uc,a,size,data):
  nonlocal lastpc
  lastpc=a
  if a in (MAIN+0x81a714,MAIN+0x81a774,MAIN+0x81a82c,MAIN+0x81ab80):trace.append(hex(a-MAIN))
 def reads(uc,access,a,size,v,data):
  if HID<=a<HID+0x400:dbgreads.append((hex(a-HID),size))
 def bad(uc,access,a,size,v,data):
  print('BAD access',hex(a),'PC',hex(lastpc),'kind',kind);return False
 u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_READ,reads);u.hook_add(UC_HOOK_MEM_INVALID,bad)
 u.emu_start(MAIN+0x81a698,STOP,count=100000)
 assert u.reg_read(UC_ARM64_REG_PC)==STOP,hex(u.reg_read(UC_ARM64_REG_PC))
 return dict(kind=kind,production=production,debugCount=struct.unpack('<Q',bytes(u.mem_read(HID+0x18,8)))[0],poison=poison,npadAxes=npad_axes,axes=struct.unpack('<6f',bytes(u.mem_read(OBJ+0x468,24))),debugOutput=struct.unpack('<QIIiiii',bytes(u.mem_read(frame+0x20,32))),trace=trace,debugReads=dbgreads)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main',required=True)
    parser.add_argument('--sdk',required=True)
    args=parser.parse_args()
    M,MS=load_nso(args.main,'a10b182dcf7bbe4712a20af54d9ae1cddd83621b88bab3ed112fb55ee1b2122f',0x5000000)
    S,SS=load_nso(args.sdk,'c88f6c116562dd1db191435b34175c934a77b1ab8009fef68533aa0405628dba',0x1100000)
    SM={s['name']:s for s in SS}
    root=Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='strato-debugpad-replay-') as tmp:
        executable=Path(tmp)/'producer-test'
        snapshot=Path(tmp)/'debugpad.bin'
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-pthread',
            '-I'+str(root/'tests/hid/include'),'-I'+str(root/'app/src/main/cpp'),
            str(root/'tests/hid/debugpad_tests.cpp'),'-o',str(executable)],check=True)
        subprocess.run([str(executable),str(snapshot)],check=True)
        PRODUCTION=snapshot.read_bytes()
    results=[]
    for kind in ['FullKey','JoyDual','Handheld']:
        for axes in [(0,0,0,0),(8192,-4096,16384,-16384)]:
            empty=test(kind,npad_axes=axes)
            fixed=test(kind,npad_axes=axes,production=True)
            clean=test(kind,npad_axes=axes,poison=(0,0))
            assert fixed['axes']==clean['axes'],(fixed,clean)
            assert fixed['axes']!=empty['axes'],(fixed,empty)
            assert fixed['debugOutput'][1:]==(0,0,0,0,0,0)
            expected={'FullKey':'0x81a714','JoyDual':'0x81a774','Handheld':'0x81a82c'}[kind]
            assert fixed['trace']==[expected,'0x81ab80']
            print(kind,'Npad',axes,'empty',empty['axes'],'production',fixed['axes'])
            results.extend([empty,fixed,clean])
    print('PASS: 18 real main + SDK executions; false axes removed, Npad input preserved.')
