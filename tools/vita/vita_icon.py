#!/usr/bin/env python3
"""Adds the Vita Settings icon (a PS Vita outline in the style of the game's options icons) to the game's global
texture pack, GLOBAL/GlobalB.lzc, as texture VITA_SETTINGS_ICON (hash 0x5654A1C0, src/ug2/menu_entry.cpp).

    vita_icon.py GlobalB.lzc GlobalB.vita.lzc preview.png

The texture goes into GlobalTextures.tpk in hash order, its pixels (128x64 DXT3) after the pack's others, and the
pack grows by a multiple of 0x1000 so everything after it keeps its alignment. Copy the result over GlobalB.lzc and
GLOBALB.BUN in the game folder; without it the Vita Settings icons show no picture.
"""
import struct, sys, numpy as np
from PIL import Image, ImageDraw

def c565(c): return ((c>>11&31)*255//31,(c>>5&63)*255//63,(c&31)*255//31)
def dxt3(buf,w,h):
    img=np.zeros((h,w,4),np.uint8); o=0
    for by in range(h//4):
        for bx in range(w//4):
            a=struct.unpack_from('<Q',buf,o)[0]; c0,c1,bits=struct.unpack_from('<HHI',buf,o+8); o+=16
            p0=np.array(c565(c0));p1=np.array(c565(c1)); pal=[p0,p1,(2*p0+p1)//3,(p0+2*p1)//3]
            for i in range(16):
                y,x=divmod(i,4); img[by*4+y,bx*4+x,:3]=pal[bits>>(2*i)&3]; img[by*4+y,bx*4+x,3]=(a>>(4*i)&15)*17
    return img
NEWHASH=0x5654A1C0
W,H=128,64
def art(template):
    """White with alpha, as the game's options icons are (the menu tints them): a PS Vita outline at 4/5, its
    screen, sticks and buttons cut out."""
    S=8
    def mask(draw):
        im=Image.new('L',(W*S,H*S),0); draw(ImageDraw.Draw(im)); return np.array(im.resize((W,H),Image.LANCZOS)).astype(int)
    # the glyph sits in the tile and runs past its foot, as the game's do
    L,T,R,B=36,17,92,47
    def body(dr,grow=0):
        g=grow*S
        dr.rounded_rectangle((L*S-g,T*S-g,R*S+g,B*S+g),radius=12*S+g,fill=255)
    def holes(dr):
        dr.rectangle(((L+15)*S,(T+5)*S,(R-15)*S,(B-5)*S),fill=255)                        # screen
        lx,rx,my=(L+7.5)*S,(R-7.5)*S,(T+10)*S
        a,b=1.4*S,4.2*S                                                                   # d-pad
        dr.rectangle((lx-a,my-b,lx+a,my+b),fill=255); dr.rectangle((lx-b,my-a,lx+b,my+a),fill=255)
        for dx,dy in ((0,-3.2),(0,3.2),(-3.2,0),(3.2,0)):                                 # face buttons
            x,y=rx+dx*S,my+dy*S; r=1.4*S; dr.ellipse((x-r,y-r,x+r,y+r),fill=255)
        for x in (lx,rx):                                                                 # sticks
            y=(B-8)*S; r=2.6*S; dr.ellipse((x-r,y-r,x+r,y+r),fill=255)
    glyph=mask(lambda dr: body(dr))
    gap=mask(lambda dr: body(dr,2))
    hole=mask(holes)
    t=template.astype(int).copy()
    # tile: the template's, its old glyph painted over at the tile's own alpha
    ta=t[:,:,3]
    # no tile: only the carousel arrows are kept
    a=np.where((np.arange(W)[None,:]<40)|(np.arange(W)[None,:]>88),ta,0)*0
    a=a*(255-gap)//255                                   # clear gap
    g=glyph*(255-hole)//255
    a=np.maximum(a,g*204//255)
    out=np.zeros((H,W,4),np.uint8); out[:,:,:3]=255; out[:,:,3]=np.clip(a,0,255)
    return out

def save_preview(img,template,path):
    # ours beside the template, tinted as the menu shows them
    row=Image.new('RGBA',(W*2+8,H),(24,28,32,255))
    for k,im in enumerate((template,img)):
        x=np.array(im).astype(float); x[:,:,0]*=0.78; x[:,:,1]*=0.94; x[:,:,2]*=0.53
        row.alpha_composite(Image.fromarray(x.astype(np.uint8)),(k*(W+8),0))
    row.resize((row.width*3,H*3),Image.NEAREST).save(path)

def enc565(c):
    r,g,b=[int(v) for v in c]; return (r*31+127)//255<<11 | (g*63+127)//255<<5 | (b*31+127)//255
def dxt3enc(img):
    h,w,_=img.shape; out=bytearray()
    for by in range(h//4):
        for bx in range(w//4):
            blk=img[by*4:by*4+4,bx*4:bx*4+4].reshape(16,4).astype(int)
            a=0
            for i in range(16): a|=int((blk[i,3]+8)//17)<<(4*i)
            cols=blk[:,:3]; vis=cols[blk[:,3]>0] if (blk[:,3]>0).any() else cols
            lum=vis@[3,6,1]; hi=vis[lum.argmax()]; lo=vis[lum.argmin()]
            c0,c1=enc565(hi),enc565(lo)
            if c0<c1: c0,c1=c1,c0; hi,lo=lo,hi
            p0=np.array(c565(c0)); p1=np.array(c565(c1))
            pal=[p0,p1,(2*p0+p1)//3,(p0+2*p1)//3]
            bits=0
            for i in range(16):
                dd=[((cols[i]-p)**2).sum() for p in pal]; bits|=int(np.argmin(dd))<<(2*i)
            if c0==c1: bits=0
            out+=struct.pack('<QHHI',a,c0,c1,bits)
    return bytes(out)

def patch(src,dst,preview):
    d=bytearray(open(src,'rb').read())
    T=0x7a80; INFO=0x7ac0; HL=0x7b4c; TI=0x80fc; FM=0xd8b0; PAD=0xef58; DATAC=0xef80; RAW=0xf000; BASE=0xf080
    assert struct.unpack_from('<I',d,T)[0]==0xb3300000 and struct.unpack_from('<I',d,RAW)[0]==0x33320002
    n=struct.unpack_from('<I',d,HL+4)[0]//8
    hs=[struct.unpack_from('<I',d,HL+8+8*i)[0] for i in range(n)]
    assert NEWHASH not in hs
    tpl=hs.index(0xd708efef)
    tinfo=bytearray(d[TI+8+124*tpl:TI+8+124*tpl+124]); fmt=bytes(d[FM+8+32*tpl:FM+8+32*tpl+32])
    off,=struct.unpack_from('<I',tinfo,48); size,=struct.unpack_from('<I',tinfo,56)
    template=dxt3(bytes(d[BASE+off:BASE+off+size]),W,H)
    img=art(template)
    save_preview(img,template,preview)
    pix=dxt3enc(img); assert len(pix)==8192
    rawsize,=struct.unpack_from('<I',d,RAW+4)
    newoff=((BASE+ (RAW+8+rawsize-BASE) + 0x7f)&~0x7f)-BASE   # appended, 0x80 aligned
    name=b'VITA_SETTINGS_ICON'
    tinfo[12:36]=name.ljust(24,b'\0'); struct.pack_into('<I',tinfo,36,NEWHASH); struct.pack_into('<I',tinfo,48,newoff)
    k=sum(1 for h in hs if h<NEWHASH)
    # rebuild the TPK
    hlist=bytearray(d[HL+8:HL+8+8*n]); hlist[8*k:8*k]=struct.pack('<II',NEWHASH,0)
    tis=bytearray(d[TI+8:TI+8+124*n]); tis[124*k:124*k]=tinfo
    fms=bytearray(d[FM+8:FM+8+32*n]); fms[32*k:32*k]=fmt
    hdr=bytes(d[INFO+8:HL])
    def ch(cid,body): return struct.pack('<II',cid,len(body))+bytes(body)
    info=hdr+ch(0x33310002,hlist)+ch(0x33310004,tis)+ch(0x33310005,fms)
    pre=bytes(d[T+8:INFO])    # the zero chunk before the info chunk
    out=bytearray(pre)+ch(0xb3310000,info)
    # padding chunk so the data chunk keeps its alignment (0x80)
    start=T+8+len(out)
    pad=(DATAC-PAD-8) ; target=(start+8+0x7f)&~0x7f
    need=(DATAC + (target - (PAD+8+ (DATAC-PAD-8))) ) # unused
    padlen=((start+8+0x7f)&~0x7f)-(start+8)
    while (start+8+padlen) % 0x80 != DATAC % 0x80: padlen+=1
    out+=struct.pack('<II',0,padlen)+b'\0'*padlen
    # data chunk: its own header chunks unchanged, the raw data grown
    datahead=bytes(d[DATAC+8:RAW])
    raw=bytearray(d[RAW+8:RAW+8+rawsize])
    raw+=b'\0'*(BASE+newoff-(RAW+8)-len(raw))+pix
    dc=datahead+ch(0x33320002,raw)
    out+=ch(0xb3320000,dc)
    tpk=ch(0xb3300000,out)
    rest=bytes(d[T+8+struct.unpack_from('<I',d,T+4)[0]:])
    # grow the TPK's tail so everything after it moves by a multiple of 0x1000
    oldlen=8+struct.unpack_from('<I',d,T+4)[0]
    grow=(len(tpk)-oldlen)
    extra=(-grow)%0x1000
    if extra:
        # pad inside the raw data chunk
        raw+=b'\0'*extra; dc=datahead+ch(0x33320002,raw); out=out[:len(out)-len(ch(0xb3320000,datahead+ch(0x33320002,raw[:-extra])))]+ch(0xb3320000,dc); tpk=ch(0xb3300000,out)
    assert (len(tpk)-oldlen)%0x1000==0
    res=bytes(d[:T])+tpk+rest
    open(dst,'wb').write(res)
    print('inserted at',k,'of',n+1,'offset',hex(newoff),'grew',len(tpk)-oldlen)

if __name__=='__main__':
    patch(sys.argv[1],sys.argv[2],sys.argv[3])
