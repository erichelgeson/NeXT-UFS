#!/usr/bin/env python3
"""Bootstrap reader for NeXT (4.3BSD old-format) UFS volumes.

Throwaway scaffolding used to extract NeXT's own headers out of a disk image
so the C implementation can be written against the real struct definitions.
"""
import struct, sys, stat, time

SBOFF, SBMAGIC, MAGIC = 8192, 0x00011954, 1372
OCG_MAGIC = 0x00090255
ROOTINO = 2

def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def s32(d, o): return struct.unpack_from('>i', d, o)[0]
def u16(d, o): return struct.unpack_from('>H', d, o)[0]

SBF = dict(sblkno=8, cblkno=12, iblkno=16, dblkno=20, cgoffset=24, cgmask=28,
           time=32, size=36, dsize=40, ncg=44, bsize=48, fsize=52, frag=56,
           minfree=60, rotdelay=64, rps=68, bmask=72, fmask=76, bshift=80,
           fshift=84, maxcontig=88, maxbpg=92, fragshift=96, fsbtodb=100,
           sbsize=104, csmask=108, csshift=112, nindir=116, inopb=120,
           nspf=124, optim=128, npsect=132, interleave=136, trackskew=140,
           headswitch=144, trkseek=148, csaddr=152, cssize=156, cgsize=160,
           ntrak=164, nsect=168, spc=172, ncyl=176, cpg=180, ipg=184, fpg=188,
           cstotal_ndir=192, cstotal_nbfree=196, cstotal_nifree=200,
           cstotal_nffree=204, cgrotor=724, cpc=856)

class Vol:
    def __init__(self, path, part=None):
        self.f = open(path, 'rb')
        self.part = part if part is not None else self._find_part()
        self.sb = self.pread(self.part + SBOFF, 2048)
        if u32(self.sb, MAGIC) != SBMAGIC:
            raise SystemExit('no superblock at 0x%x' % (self.part + SBOFF))
        for k, o in SBF.items():
            setattr(self, k, s32(self.sb, o))
        self.iblksz = 128

    def pread(self, off, n):
        self.f.seek(off); return self.f.read(n)

    def _find_part(self):
        chunk = self.f.read(64 << 20)
        i = chunk.find(struct.pack('>I', SBMAGIC))
        while i >= 0:
            p = i - MAGIC - SBOFF
            if p >= 0 and p % 512 == 0:
                return p
            i = chunk.find(struct.pack('>I', SBMAGIC), i + 1)
        raise SystemExit('no UFS superblock found')

    # --- addressing (fs_fsize-sized fragments; fsbtodb is 0 on NeXT) ---
    def foff(self, frag): return self.part + frag * self.fsize
    def cgbase(self, c): return self.fpg * c
    def cgstart(self, c):
        return self.cgbase(c) + self.cgoffset * (c & ~self.cgmask)
    def cgimin(self, c): return self.cgstart(c) + self.iblkno
    def itof(self, ino):
        return self.cgimin(ino // self.ipg) + \
            (((ino % self.ipg) * self.iblksz) // self.fsize) * self.frag \
            if False else self.cgimin(ino // self.ipg) + \
            ((ino % self.ipg) // self.inopb) * self.frag

    def cg(self, c):
        return self.pread(self.foff(self.cgstart(c) + self.cblkno), self.cgsize)

    def inode(self, ino):
        base = self.foff(self.itof(ino))
        off = base + ((ino % self.inopb) * self.iblksz)
        return Inode(self, ino, self.pread(off, 128))

    def blkmap(self, ino):
        """Yield fragment addresses for every logical block of the file."""
        d = ino.raw
        nib = self.nindir
        for i in range(12):
            yield u32(d, 40 + 4*i)
        def indir(blk, level):
            if blk == 0:
                cnt = nib ** level
                for _ in range(cnt): yield 0
                return
            buf = self.pread(self.foff(blk), self.bsize)
            for j in range(nib):
                b = u32(buf, 4*j)
                if level == 1: yield b
                else: yield from indir(b, level - 1)
        for lvl in (1, 2, 3):
            yield from indir(u32(d, 88 + 4*(lvl-1)), lvl)

    def read_file(self, ino):
        out, left = bytearray(), ino.size
        fpb = self.frag
        for lbn, blk in enumerate(self.blkmap(ino)):
            if left <= 0: break
            n = min(left, self.bsize)
            out += self.pread(self.foff(blk), n) if blk else bytes(n)
            left -= n
        return bytes(out[:ino.size])

    def readdir(self, ino):
        d = self.read_file(ino)
        o = 0
        while o < len(d):
            i, rl, nl = u32(d, o), u16(d, o+4), u16(d, o+6)
            if rl <= 0: break
            if i: yield i, d[o+8:o+8+nl].decode('latin-1')
            o += rl

    def readlink(self, ino):
        """NeXT stores symlinks <= 60 bytes inline in di_db/di_ib (fast symlink)."""
        if ino.blocks == 0 and ino.size <= 60:
            return ino.raw[40:40 + ino.size].decode('latin-1')
        return self.read_file(ino).decode('latin-1')

    def lookup(self, path, _depth=0):
        if _depth > 8: return None
        ino = self.inode(ROOTINO)
        parts = [p for p in path.split('/') if p]
        for n, part in enumerate(parts):
            for i, name in self.readdir(ino):
                if name == part:
                    ino = self.inode(i); break
            else:
                return None
            if ino.islnk():
                import posixpath
                tgt = self.readlink(ino)
                if not tgt.startswith('/'):
                    tgt = posixpath.join('/' + '/'.join(parts[:n]), tgt)
                tgt = posixpath.normpath(tgt)
                rest = '/'.join(parts[n+1:])
                return self.lookup(posixpath.join(tgt, rest), _depth + 1)
        return ino

class Inode:
    def __init__(self, vol, num, raw):
        self.vol, self.num, self.raw = vol, num, raw
        self.mode, self.nlink = u16(raw, 0), u16(raw, 2)
        self.uid, self.gid = u16(raw, 4), u16(raw, 6)
        self.size = struct.unpack_from('>Q', raw, 8)[0]
        self.atime, self.mtime, self.ctime = u32(raw,16), u32(raw,24), u32(raw,32)
        self.blocks, self.gen = u32(raw, 104), u32(raw, 108)
    def isdir(self): return stat.S_ISDIR(self.mode)
    def islnk(self): return stat.S_ISLNK(self.mode)

def main():
    ap = sys.argv
    if len(ap) < 2:
        print(__doc__); sys.exit(1)
    v = Vol(ap[1])
    cmd = ap[2] if len(ap) > 2 else 'info'
    if cmd == 'info':
        print('partition  0x%x' % v.part)
        for k in ('size','dsize','ncg','bsize','fsize','frag','cpg','ipg','fpg',
                  'csaddr','cssize','cgsize','ntrak','nsect','spc','ncyl','cpc',
                  'sblkno','cblkno','iblkno','dblkno','cgoffset','inopb','nindir'):
            print('%-10s %d' % (k, getattr(v, k)))
        print('cgmask     0x%08x' % (v.cgmask & 0xffffffff))
        print('cstotal    ndir=%d nbfree=%d nifree=%d nffree=%d' % (
            v.cstotal_ndir, v.cstotal_nbfree, v.cstotal_nifree, v.cstotal_nffree))
        cg0 = v.cg(0)
        print('ocg0 magic 0x%08x (%s)' % (u32(cg0, 980),
              'ok' if u32(cg0, 980) == OCG_MAGIC else 'BAD'))
    elif cmd == 'ls':
        ino = v.lookup(ap[3] if len(ap) > 3 else '/')
        if not ino: sys.exit('not found')
        for i, name in v.readdir(ino):
            c = v.inode(i)
            print('%7d %6o %5d %10d  %s' % (i, c.mode, c.nlink, c.size, name))
    elif cmd == 'readlink':
        print(v.readlink(v.lookup(ap[3])))
    elif cmd == 'cat':
        ino = v.lookup(ap[3])
        if not ino: sys.exit('not found')
        sys.stdout.buffer.write(v.read_file(ino))
    elif cmd == 'find':
        def walk(ino, pfx):
            for i, name in v.readdir(ino):
                if name in ('.', '..'): continue
                c = v.inode(i)
                p = pfx + '/' + name
                print(p)
                if c.isdir(): walk(c, p)
        walk(v.lookup('/'), '')

main()
