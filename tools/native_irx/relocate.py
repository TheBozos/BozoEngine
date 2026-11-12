"""Offline ELF32 IRX relocation for statically compiled native libraries. No guest execution."""
from pathlib import Path
import json,struct
S=struct.Struct('<10I')
def relocate(path,base,out):
    path=Path(path); b=path.read_bytes()
    if b[:7]!=b'\x7fELF\x01\x01\x01': raise ValueError('expected ELF32 little endian')
    phoff,shoff=struct.unpack_from('<II',b,28)
    phsize,phnum,shsize,shnum,shstr=struct.unpack_from('<5H',b,42)
    segs=[struct.unpack_from('<8I',b,phoff+i*phsize) for i in range(phnum)]
    segs=[s for s in segs if s[0]==1]
    size=(max(s[2]+s[5] for s in segs)+255)&~255
    image=bytearray(size)
    for s in segs: image[s[2]:s[2]+s[4]]=b[s[1]:s[1]+s[4]]
    secs=[S.unpack_from(b,shoff+i*shsize) for i in range(shnum)]
    names=b[secs[shstr][4]:secs[shstr][4]+secs[shstr][5]] if shstr<len(secs) else b''
    gp=0; entry=struct.unpack_from('<I',b,24)[0]
    def rd(a): return struct.unpack_from('<I',image,a)[0]
    def wr(a,v): struct.pack_into('<I',image,a,v&0xffffffff)
    for sec in secs:
        name=names[sec[0]:].split(b'\0')[0]
        if sec[1]==0x70000080 or name==b'.iopmod':
            entry,gp=struct.unpack_from('<II',b,sec[4]+4)
        if sec[1]!=9: continue
        pending=[]
        for off in range(sec[4],sec[4]+sec[5],8):
            addr,info=struct.unpack_from('<II',b,off); kind=info&255; symbol=info>>8
            if addr+4>len(image): raise ValueError('relocation outside image')
            word=rd(addr)
            if kind==0: continue
            if kind in (2,3): wr(addr,word+base)
            elif kind==4: wr(addr,(word&0xfc000000)|((((word&0x3ffffff)<<2)+base)>>2)&0x3ffffff)
            elif kind==5: pending.append((addr,word,symbol))
            elif kind==6:
                low=struct.unpack('<h',struct.pack('<H',word&65535))[0]
                for hi,high,sym in list(pending):
                    if sym==symbol:
                        value=((high&65535)<<16)+low+base
                        wr(hi,(high&0xffff0000)|((value+0x8000)>>16)&65535)
                        pending.remove((hi,high,sym))
                wr(addr,(word&0xffff0000)|((low+base)&65535))
            elif kind==1: wr(addr,(word&0xffff0000)|((word+base)&65535))
            else: raise ValueError(f'unsupported relocation {kind}')
        if pending: raise ValueError('unpaired HI16 relocation')
    hash=0xcbf29ce484222325
    for v in b: hash=((hash^v)*0x100000001b3)&0xffffffffffffffff
    info=dict(name=path.name+';1',base=base,size=size,entry=base+entry,gp=base+gp,hash=f'{hash:016x}')
    out=Path(out);out.parent.mkdir(parents=True,exist_ok=True)
    out.with_suffix('.bin').write_bytes(image)
    out.with_suffix('.json').write_text(json.dumps(info))
    return info
if __name__=='__main__':
    import sys
    print(relocate(sys.argv[1],int(sys.argv[2],0),sys.argv[3]))
