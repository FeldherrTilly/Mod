import os,sys,zlib,struct; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png import write_png
x0,x1,y0,y1,out=int(sys.argv[1]),int(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4]),sys.argv[5]
data=open(sys.argv[6] if len(sys.argv) > 6 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out', 'preview.png'),'rb').read(); i=8; idat=b''
while i<len(data):
    n=struct.unpack('>I',data[i:i+4])[0]; t=data[i+4:i+8]; c=data[i+8:i+8+n]
    if t==b'IHDR': w,h=struct.unpack('>II',c[:8])
    if t==b'IDAT': idat+=c
    i+=12+n
raw=zlib.decompress(idat)
rows=[[tuple(raw[y*(1+3*w)+1+k:y*(1+3*w)+4+k]) for k in range(0,3*w,3)] for y in range(h)]
S=8; MH=h//S; crop=[row[x0*S:x1*S] for row in rows[(MH-y1)*S:(MH-y0)*S]]
big=[[p for p in row for _ in range(3)] for row in crop for _ in range(3)]
write_png(out,len(big[0]),len(big),big)
