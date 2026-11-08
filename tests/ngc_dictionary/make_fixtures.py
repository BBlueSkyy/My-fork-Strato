"""Generate synthetic words in the firmware's succinct Aho-Corasick format.
No Nintendo word lists or other firmware content is included.
"""
import gzip
import pathlib
import struct
import sys

p32 = lambda x: struct.pack('<I', x)
p64 = lambda x: struct.pack('<Q', x)
def vector(bits):
    words = [sum(bit << j for j, bit in enumerate(bits[i:i+32])) for i in range(0, len(bits), 32)]
    return p32(len(bits)) + b''.join(map(p32, words))
def rank(bits):
    if not bits:
        return b''
    blocks = [0] * (len(bits)//256+1)
    sub = [0] * (len(bits)//32+1)
    for i in range((len(bits)+31)//32):
        if i % 8 == 0:
            blocks[i//8] = sum(bits[:i*32])
        sub[i] = sum(bits[i//8*256:i*32])
    return b''.join(map(p32, blocks)) + bytes(sub)
def select(bits):
    words = [bits[i:i+32] for i in range(0,len(bits),32)]
    nonzero = [i for i,w in enumerate(words) if any(w)]
    seq = [int(i == 0 or nonzero[i-1]+1 != pos) for i,pos in enumerate(nonzero)]
    offsets = [pos-i for i,pos in enumerate(nonzero) if seq[i]]
    starts = []
    for pos in nonzero:
        starts += [1] + [0] * (sum(words[pos])-1)
    return p32(len(nonzero)) + b''.join(map(p32, offsets + [0]*(len(nonzero)-len(offsets)))) + vector(seq)+vector(starts)+rank(seq)+rank(starts)
def set_bits(bits): return vector(bits)+rank(bits)
def sparse(values):
    if not values:
        bits, low, width = [], [], 1
    else:
        width = max(0, (values[-1]//len(values)).bit_length()-1)
        bits = [0]*(len(values)+(values[-1]>>width)+1)
        for i,v in enumerate(values): bits[(v>>width)+i] = 1
        packed = sum((v & ((1<<width)-1)) << (i*width) for i,v in enumerate(values))
        low = [(packed >> i) & 0xffffffff for i in range(0,max(1,len(values)*width),32)]
    return p64(values[-1]+1 if values else 0)+p64(values[0] if values else 0)+p64(values[-1] if values else 0)+p32(len(values))+p32(width)+p32(len(low))+b''.join(map(p32,low))+set_bits(bits)+select(bits)
def array(values):
    ranges, fields = [], []
    while len(values) >= 64:
        chunk, values = values[:64], values[64:]
        base = max(chunk)
        width = max(1, (base-min(chunk)).bit_length()+1)
        ranges.extend([((width-1)<<27)|len(fields), base])
        packed = sum(((v-base)&((1<<width)-1)) << (i*width) for i,v in enumerate(chunk))
        fields.extend((packed>>i)&0xffffffff for i in range(0,64*width,32))
    return p32(len(ranges))+b''.join(map(p32,ranges))+p32(len(fields))+b''.join(map(p32,fields))+bytes([len(values)])+b''.join(map(p32,values))
def trie(words):
    paths = {''}
    for word in words:
        paths.update(word[:i] for i in range(1,len(word)+1))
    paths = sorted(paths, key=lambda x:x[::-1])
    ids = {v:i for i,v in enumerate(paths)}
    n = len(paths)
    edges = sorted(n*ord(path[-1])+ids[path[:-1]] for path in paths if path)
    assert [ids[path] for path in sorted(paths[1:],key=lambda p:n*ord(p[-1])+ids[p[:-1]])] == list(range(1,n))
    # The BP tree stores Aho-Corasick failure links in node-id/preorder order.
    children = {i:[] for i in range(n)}
    for path in paths[1:]:
        suffix = path[1:]
        while suffix not in ids: suffix = suffix[1:]
        children[ids[suffix]].append(ids[path])
    bits=[]; order=[]
    def visit(node):
        order.append(node); bits.append(1)
        for child in children[node]: visit(child)
        bits.append(0)
    visit(0)
    assert order == list(range(n)), order
    wordids = sorted(ids[word] for word in words)
    return array([len(paths[i]) for i in wordids])+sparse(wordids)+array([])+sparse([])+p32(n)+sparse(edges)+set_bits(bits)+bytes([0])+select(bits)

def main():
    root = pathlib.Path(sys.argv[1]); root.mkdir(parents=True,exist_ok=True)
    for region,partial,complete,delimited in [
        ('common',['zork']+[f'token{i:02d}'+('x'*(i%3)) for i in range(70)],[],[]), ('0',['bad','dab'],['word'],[r'\bword\b']), ('1',['regional'],[],[])]:
        for suffix,words in [('not_b',partial),('b1',complete),('b2',delimited)]:
            (root/f'ac_{region}_{suffix}_nx').write_bytes(gzip.compress(trie(words),mtime=0))
if __name__ == '__main__': main()
