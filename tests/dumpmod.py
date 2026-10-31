import sys
f=open(sys.argv[1],'rb').read()
nch=int(sys.argv[3]) if len(sys.argv)>3 else 4
rows=int(sys.argv[2]) if len(sys.argv)>2 else 16
print(f[1080:1084], 'len',f[950], list(f[952:952+f[950]]))
for i in range(31):
    h=f[20+i*30:50+i*30]
    if h[22] or h[23]: print('smp',i+1,h[:22].split(b'\0')[0], 'len',((h[22]<<8)|h[23])*2,'ft',h[24]&15,'vol',h[25],'loop',((h[26]<<8)|h[27])*2,((h[28]<<8)|h[29])*2)
for r in range(rows):
    row=[]
    for c in range(nch):
        o=1084+r*4*nch+c*4
        per=((f[o]&15)<<8)|f[o+1]; s=(f[o]&0xF0)|(f[o+2]>>4); fx=f[o+2]&15; pa=f[o+3]
        row.append(f"{per:4d} {s:02d} {fx:X}{pa:02X}")
    print(f"{r:2d}", ' | '.join(row))
