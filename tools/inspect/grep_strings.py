import re,sys
data=open(sys.argv[1],'rb').read()
pat=re.compile(sys.argv[2].encode(),re.I)
seen=set()
for m in re.finditer(rb'[\x20-\x7e]{8,}',data):
    s=m.group()
    if pat.search(s) and s not in seen:
        seen.add(s); print(s.decode()[:200])
