#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lists and extracts files of the game's Alchemy IGA archives (archives/*.pak, version 13), for
research on your own dump (TODO.md, Grand Prix intros). Work in progress: small files of
update.pak extract; offsets above 4 GiB (update2.pak) and some multi-block files do not yet.

  iga_extract.py ARCHIVE list SUBSTRING     the files whose path contains SUBSTRING (and sizes)
  iga_extract.py ARCHIVE PATH OUTPUT        extract one file

Layout (as read from the files): "IGA\\x1a", version 13, toc size, file count, sector size
(0x800), hash divider and slop, counts of large (u32) / medium (u16) / small (u8) block entries,
name table offset (u64) and size; then the sorted name hashes (u32 each), 16-byte file entries
(offset, ordinal, length, block: compression in the top 4 bits, first block index below), the
block tables (sector offset of each 32 KiB piece from the file start, top bit: compressed). A
compressed piece is a u32 size, the 5 LZMA properties, then a raw LZMA1 stream. The name table
holds, per file, an offset to the build path and the game path after it.
"""
import struct,sys,lzma,os
def open_iga(path):
    f=open(path,'rb'); h=f.read(0x38)
    ver,toc,n,sector,div,slop,nl,nm,ns=struct.unpack_from('<9I',h,4)
    nto=struct.unpack_from('<I',h,0x28)[0] | (0 if True else 0)
    nto=struct.unpack_from('<Q',h,0x28)[0] & 0xffffffffff
    nts=struct.unpack_from('<I',h,0x30)[0]
    f.seek(0x38); hashes=f.read(4*n); infos=f.read(16*n)
    large=f.read(4*nl); medium=f.read(2*nm); small=f.read(ns)
    f.seek(nto); nt=f.read(nts); offs=struct.unpack_from(f'<{n}I',nt,0)
    files={}
    for i in range(n):
        o=offs[i]; a=nt.index(b'\0',o); b=nt.index(b'\0',a+1)
        name=nt[a+1:b].decode()
        off,ordinal,length,block=struct.unpack_from('<4I',infos,16*i)
        files[name.lower()]=(off,length,block,name)
    return f,files,sector,large,medium,small,nl,nm
def read(iga,name):
    f,files,sector,large,medium,small,nl,nm=iga
    off,length,block,real=files[name.lower()]
    comp=block>>28; idx=block&0xfffffff
    if block==0xffffffff or comp==0xf or (block>>24)==0xff:
        f.seek(off); return f.read(length)
    out=bytearray(); chunks=(length+0x7fff)//0x8000
    for k in range(chunks):
        j=idx+k
        if j<nl: e=struct.unpack_from('<I',large,4*j)[0]; flag=e>>31; sec=e&0x7fffffff
        elif j<nl+nm: e=struct.unpack_from('<H',medium,2*(j-nl))[0]; flag=e>>15; sec=e&0x7fff
        else: e=small[j-nl-nm]; flag=e>>7; sec=e&0x7f
        want=min(0x8000,length-k*0x8000)
        f.seek(off+sec*sector)
        if flag:
            csize=struct.unpack('<I',f.read(4))[0]; props=f.read(5)
            lc=props[0]%9; lp=(props[0]//9)%5; pb=props[0]//45; dsz=struct.unpack_from('<I',props,1)[0]
            dec=lzma.LZMADecompressor(lzma.FORMAT_RAW,filters=[{'id':lzma.FILTER_LZMA1,'dict_size':max(dsz,4096),'lc':lc,'lp':lp,'pb':pb}])
            out+=dec.decompress(f.read(csize-5 if csize>5 else csize),max_length=want)[:want]
        else:
            out+=f.read(want)
    return bytes(out)
if __name__=='__main__':
    iga=open_iga(sys.argv[1])
    if sys.argv[2]=='list':
        for k in iga[1]:
            if sys.argv[3].lower() in k: print(iga[1][k][3], iga[1][k][1])
    else:
        d=read(iga,sys.argv[2]); open(sys.argv[3],'wb').write(d); print(len(d))
