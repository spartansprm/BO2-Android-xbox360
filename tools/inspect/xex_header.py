import sys; sys.stdout.reconfigure(encoding="utf-8")
import struct,sys
for p in sys.argv[1:]:
    d=open(p,'rb').read(0x8000)
    magic,flags,peoff,res,secoff,n=struct.unpack('>4sIIIII',d[:24])
    print('==',p.split('/')[-1],magic,'flags',hex(flags),'pe_off',hex(peoff),'nhdr',n)
    for i in range(n):
        k,v=struct.unpack('>II',d[24+i*8:32+i*8])
        line=f'  key {k:08x} val {v:08x}'
        if k==0x00018002: line+=' (entry point)'
        if k==0x00010201: line+=' (image base)'
        if k==0x00040006:
            ex=d[v:v+24]; mid,ver,base,tid=struct.unpack('>IIII',ex[:16])
            line+=f' (exec info: media {mid:08x} ver {ver:08x} base {base:08x} titleid {tid:08x})'
        if k==0x000103FF:
            sz=struct.unpack('>I',d[v:v+4])[0]; s=d[v+4:v+4+min(sz,2000)]
            line+=' (imports) '+repr([x.decode(errors="ignore") for x in s.split(b'\0') if len(x)>3][:8])
        if k==0x000183FF: line+=' (original pe name) '+d[v+4:v+64].split(b'\0')[0].decode(errors='ignore')
        if k==0x00030000: line+=' (system flags)'
        if k==0x00020104: line+=f' (tls info)'
        if k==0x00020200: line+=f' (default stack size)'
        print(line)
