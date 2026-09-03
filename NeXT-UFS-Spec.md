# The NeXT disk format

Everything needed to read and write a NeXTSTEP or OPENSTEP disk, written down
because no single document covers it and several of the details below cost real
debugging time to find.

Structure layouts come from NeXT's own headers, which are in
`docs/next-headers/` — pulled straight out of an OPENSTEP 4.2 disk image. Every
offset and rule here was then checked against two real volumes, and the result
was validated by NeXTSTEP's own `fsck` running in the Previous emulator. Where
something is inferred rather than confirmed, it says so.

Reference volumes used throughout:

| | HD1 | HD00 |
|---|---|---|
| Origin | NeXTSTEP, built by Previous | OPENSTEP 4.2 install |
| Size | 110,100,480 bytes | 2,147,549,184 bytes |
| Partition a | byte 0x28000 | byte 0x28000 |
| `fs_size` | 107,360 frags | 2,097,056 frags |
| `fs_ncg` | 105 | 469 |

---

## 1. Shape of a disk

    byte 0          disk label (four copies)
    ...             boot blocks
    byte 163840     partition a: a 4.3BSD filesystem
      +0            boot block area (8KB)
      +8192         superblock
      +16384        superblock backup (cylinder group 0's copy)
      ...           cylinder groups

The 160 sectors before partition a are the *front porch*: the label, its copies
and the two boot blocks live there. Partition offsets in the label are relative
to the end of it.

Everything on disk is **big-endian** on m68k hardware. NeXTSTEP for Intel writes
the same structures little-endian; the label is separate and is discussed in
§10.

---

## 2. The disk label

A `struct disk_label` (`bsd/dev/disk_label.h`) sits at byte 0, with three more
copies at **0x1e00, 0x3c00 and 0x5a00**. Both reference volumes carry exactly
four. It is laid out for the m68k ABI, where a 32-bit field needs only 2-byte
alignment — this matters, and is why the partition array does not start on a
4-byte boundary.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `dl_version` | `"dlV3"`, `"dlV2"` or `"NeXT"` (v1) |
| 4 | 4 | `dl_label_blkno` | block this label is at; 0 on both references |
| 8 | 4 | `dl_size` | media size in sectors; **0 on both references** |
| 12 | 24 | `dl_label` | volume name, e.g. `BlueSCSIToolbox` |
| 36 | 4 | `dl_flags` | `0x80000000` = uninitialised |
| 40 | 4 | `dl_tag` | volume tag |
| 44 | 514 | `dl_dt` | `struct disktab`, below |
| 558 | 2 | `dl_v3_checksum` | v3 only |
| 558 | 6680 | `dl_bad[1670]` | v1/v2 bad block table |
| 7238 | 2 | `dl_checksum` | v1/v2 only |

`dl_size` being zero on both real disks means **do not rely on it**; take the
media size from the file or device.

### 2.1 `struct disktab`, at label offset 44

| Offset | Size | Field | HD1 |
|---|---|---|---|
| 44 | 24 | `d_name` | `PreviousHDD-512` |
| 68 | 24 | `d_type` | `fixed_rw_scsi` |
| 92 | 4 | `d_secsize` | 1024 |
| 96 | 4 | `d_ntracks` | 4 |
| 100 | 4 | `d_nsectors` | 16 |
| 104 | 4 | `d_ncylinders` | 1680 |
| 108 | 4 | `d_rpm` | 3600 |
| 112 | 2 | `d_front` | 160 |
| 114 | 2 | `d_back` | 0 |
| 116 | 2 | `d_ngroups` | 0 |
| 118 | 2 | `d_ag_size` | 0 |
| 120 | 2 | `d_ag_alts` | 0 |
| 122 | 2 | `d_ag_off` | 0 |
| 124 | 8 | `d_boot0_blkno[2]` | 32, 96 |
| 132 | 24 | `d_bootfile` | `sdmach` |
| 156 | 32 | `d_hostname` | `previous` |
| 188 | 1 | `d_rootpartition` | `a` |
| 189 | 1 | `d_rwpartition` | `b` |
| 190 | 368 | `d_partitions[8]` | 46 bytes each |

**The sector size is 1024 bytes**, not 512. This is the single most common way
to get lost: `d_secsize`, `d_front`, `p_base` and `p_size` are all counted in
1024-byte sectors on a NeXT hard disk.

### 2.2 `struct partition`, 46 bytes each from offset 190

| Offset | Size | Field | Partition a on HD1 |
|---|---|---|---|
| 0 | 4 | `p_base` | 0 (sectors, past the front porch) |
| 4 | 4 | `p_size` | 107360 sectors |
| 8 | 2 | `p_bsize` | 8192 |
| 10 | 2 | `p_fsize` | 1024 |
| 12 | 1 | `p_opt` | `s` or `t` (space/time) |
| 14 | 2 | `p_cpg` | 16 |
| 16 | 2 | `p_density` | 4096 bytes per inode |
| 18 | 1 | `p_minfree` | 5 |
| 19 | 1 | `p_newfs` | 1 |
| 20 | 16 | `p_mountpt` | |
| 36 | 1 | `p_automnt` | |
| 37 | 8 | `p_type` | `4.3BSD` |

An unused partition is all `0xff` — check `p_base < 0` or `p_size <= 0`.

The byte offset of a partition is:

    (d_front + p_base) * d_secsize

which is `(160 + 0) * 1024 = 0x28000` on both reference disks.

### 2.3 Checksum

A ones-complement sum of big-endian 16-bit words from the start of the label up
to (not including) the checksum field, with the carries folded back in:

```c
uint16_t checksum(const uint8_t *buf, int limit)
{
	unsigned sum = 0;
	for (int i = 0; i + 1 < limit; i += 2)
		sum += ((unsigned)buf[i] << 8) + buf[i + 1];
	sum = (sum & 0xffff) + (sum >> 16);
	sum += sum >> 16;
	return sum & 0xffff;
}
```

`limit` is 558 for `dlV3` and 7238 for `dlV2`/`NeXT`. Verified: HD1's stored
checksum is `0x279a` and this reproduces it. The label is big-endian even on
little-endian NeXT hardware, since the ROM writes it.

---

## 3. The filesystem

Inside the partition is a 4.3BSD fast filesystem: the *original* one, not the
4.4BSD/FreeBSD descendant. Static cylinder groups, 128-byte inodes, 16-bit uids,
`d_namlen` in the directory entry and no `d_type`.

Both reference volumes and everything NeXT's `newfs` makes use:

| | Value |
|---|---|
| `fs_bsize` | 8192 |
| `fs_fsize` | 1024 |
| `fs_frag` | 8 |
| `fs_fsbtodb` | 0 |
| `fs_nspf` | 1 |
| `fs_sbsize` | 2048 |

`fs_fsbtodb == 0` combined with a 1024-byte sector means a *filesystem
fragment*, a *disk block* and a *sector* are all the same 1024 bytes. So a
fragment address converts to a byte offset as:

    byte = partition_offset + frag * fs_fsize

That holds everywhere and is worth internalising: no `fsbtodb` shifting is ever
needed for a normal NeXT hard disk.

### 3.1 Where the superblock is

NeXT's `fs.h` has a real fork in it:

```c
#if	NeXT
#define	SBLOCK	((daddr_t)(BBLOCK + BBSIZE))		/* 8192, a BYTE offset */
#else
#define	SBLOCK	((daddr_t)(BBLOCK + BBSIZE) / DEV_BSIZE)
#endif
```

So the primary superblock is at **byte 8192** of the partition — fragment 8.
Meanwhile `fs_sblkno` is **16**, which is the per-cylinder-group backup offset
in fragments. Both are true at once and they are not the same location:

| Volume | superblocks found at fragment |
|---|---|
| HD1 | 8, 16, 1056, 2096, … |
| HD00 | 8, 16, 4568, … |

Fragment 8 is the one the kernel mounts. Fragment 16 is cylinder group 0's
backup, and `cgsblock(c)` gives the rest. `mkfs` must write both.

### 3.2 `struct fs` field offsets

Magic `0x00011954` at offset 1372 is how you find one.

| Offset | Field | | Offset | Field |
|---|---|---|---|---|
| 8 | `fs_sblkno` | | 128 | `fs_optim` |
| 12 | `fs_cblkno` | | 152 | `fs_csaddr` |
| 16 | `fs_iblkno` | | 156 | `fs_cssize` |
| 20 | `fs_dblkno` | | 160 | `fs_cgsize` |
| 24 | `fs_cgoffset` | | 164 | `fs_ntrak` |
| 28 | `fs_cgmask` | | 168 | `fs_nsect` |
| 32 | `fs_time` | | 172 | `fs_spc` |
| 36 | `fs_size` | | 176 | `fs_ncyl` |
| 40 | `fs_dsize` | | 180 | `fs_cpg` |
| 44 | `fs_ncg` | | 184 | `fs_ipg` |
| 48 | `fs_bsize` | | 188 | `fs_fpg` |
| 52 | `fs_fsize` | | 192 | `fs_cstotal` (4 × 4 bytes) |
| 56 | `fs_frag` | | 208 | `fs_fmod` (char) |
| 60 | `fs_minfree` | | 209 | **`fs_state`** (char) |
| 64 | `fs_rotdelay` | | 210 | `fs_ronly` (char) |
| 68 | `fs_rps` | | 211 | `fs_flags` (char) |
| 72 | `fs_bmask` | | 212 | `fs_fsmnt[512]` |
| 76 | `fs_fmask` | | 724 | `fs_cgrotor` |
| 80 | `fs_bshift` | | 728 | `fs_csp[32]` (in-core only) |
| 84 | `fs_fshift` | | 856 | `fs_cpc` |
| 88 | `fs_maxcontig` | | 860 | `fs_postbl[32][8]` (int16) |
| 92 | `fs_maxbpg` | | 1372 | `fs_magic` |
| 96 | `fs_fragshift` | | 1376 | `fs_rotbl[]` (uint8) |
| 100 | `fs_fsbtodb` | | | |
| 104 | `fs_sbsize` | | | |
| 108 | `fs_csmask` | | | |
| 112 | `fs_csshift` | | | |
| 116 | `fs_nindir` | | | |
| 120 | `fs_inopb` | | | |
| 124 | `fs_nspf` | | | |

**`fs_state` at offset 209 is NeXT-specific.** 4.4BSD has `fs_clean` there.
Values: 1 clean, 2 dirty, 3 corrupted (mounted while dirty). Both reference
volumes read 1. Leave a volume at 1 after writing to it, or NeXT will insist on
checking it. A mount sets 2 for as long as it lasts and puts 1 back when it
ends, so a volume left behind by a crash is checked before it is used.

`fs_dsize` is the number of fragments available for data, and it is exact:

    fs_dsize = fs_size
             - (fs_dblkno + howmany(fs_cssize, fs_fsize))   /* group 0 */
             - (fs_ncg - 1) * (fs_dblkno - fs_sblkno)       /* every other group */

Verified against both references to the fragment (103,142 and 2,025,744).
Leaving it zero makes `fsck` report `+Infinity% fragmentation`, which is a
useful tell.

### 3.3 Locating a cylinder group

```c
cgbase(c)   = fs_fpg * c
cgstart(c)  = cgbase(c) + fs_cgoffset * (c & ~fs_cgmask)
cgsblock(c) = cgstart(c) + fs_sblkno    /* superblock backup */
cgtod(c)    = cgstart(c) + fs_cblkno    /* the cg header */
cgimin(c)   = cgstart(c) + fs_iblkno    /* inode blocks */
cgdmin(c)   = cgstart(c) + fs_dblkno    /* first data fragment */
```

Both references use `fs_cgoffset = 16` and `fs_cgmask = 0xfffffffc`, so groups
are staggered 0, 16, 32, 48, 0, 16, … fragments past their base.

`fs_sblkno` 16, `fs_cblkno` 24 and `fs_iblkno` 32 are constant across both
volumes, and:

    fs_dblkno = fs_iblkno + (fs_ipg / fs_inopb) * fs_frag

HD1: 32 + (192/64)·8 = 56. HD00: 32 + (1088/64)·8 = 168.

The cylinder group summary array lives at `fs_csaddr`, which is `cgdmin(0)`,
and holds `fs_ncg` × 16 bytes: one `struct csum` per group, mirroring each
group's own `cg_cs`.

---

## 4. Cylinder groups

The old, static `struct cg` — fixed-size arrays, magic near the end. NeXT's
`fs.h` still calls it `struct cg`; Linux calls the same layout `ufs_old_cylinder_group`
and gates it behind `UFS_CG_OLD`. FreeBSD and NetBSD no longer support it at
all, which is the main reason their tools cannot touch a NeXT disk.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `cg_link` | unused on disk |
| 4 | 4 | `cg_rlink` | unused on disk |
| 8 | 4 | `cg_time` | |
| 12 | 4 | `cg_cgx` | group number |
| 16 | 2 | `cg_ncyl` | cylinders in this group |
| 18 | 2 | `cg_niblk` | **inodes**, not inode blocks: equals `fs_ipg` |
| 20 | 4 | `cg_ndblk` | data fragments in this group |
| 24 | 16 | `cg_cs` | ndir, nbfree, nifree, nffree |
| 40 | 4 | `cg_rotor` | |
| 44 | 4 | `cg_frotor` | |
| 48 | 4 | `cg_irotor` | |
| 52 | 32 | `cg_frsum[8]` | free runs of each fragment length |
| 84 | 128 | `cg_btot[32]` | free whole blocks per cylinder |
| 212 | 512 | `cg_b[32][8]` | int16, free blocks per cylinder × rotational position |
| 724 | 256 | `cg_iused[256]` | inode bitmap, set = in use |
| 980 | 4 | `cg_magic` | `0x00090255` |
| 984 | … | `cg_free[]` | fragment bitmap, **set = free** |

The magic at 980 is the reliable way to tell an old cylinder group from a modern
one, whose magic is at offset 4.

The first ~52 bytes coincidentally match the modern layout, which is why code
written for 4.4BSD appears to read a NeXT group correctly and then writes
garbage into `cg_frsum` when it stores the rotors.

`cg_niblk` holds the inode *count* (192 on HD1, 1088 on HD00), despite the name
and the comment in the header saying "number of inode blocks".

### 4.1 The fragment bitmap

One bit per fragment, LSB first within each byte, **a set bit means free**.

Bit *i* refers to fragment `cgbase(c) + i` — relative to `cgbase`, not
`cgstart`. Getting this wrong is subtle: it only shows up in groups where the
staggering is non-zero.

Consequently the fragments between `cgbase(c)` and `cgsblock(c)` — 0, 16, 32 or
48 of them depending on the stagger — are ordinary data fragments and are marked
free. The metadata that is marked in use is:

    group 0:  [0, fs_dblkno + howmany(fs_cssize, fs_fsize))    /* boot blocks too */
    group c:  [cgstart(c) - cgbase(c) + fs_sblkno,
               cgstart(c) - cgbase(c) + fs_dblkno)

Group 0 additionally has its boot area in use; every other group leaves its
`dlower` region free. Confirmed on both references, in every group.

### 4.2 The last cylinder group

The last group is normally short, and **`cg_ncyl` for it is `fs_ncyl % fs_cpg`**:

| Volume | `fs_ncyl` | `fs_cpg` | last `cg_ncyl` | last `cg_ndblk` |
|---|---|---|---|---|
| HD1 | 1678 | 16 | 14 | 864 |
| HD00 | 7490 | 16 | 2 | 416 |

`fsck` rebuilds each group's header and compares it, and it computes that field
exactly this way — including yielding **zero** when the size divides evenly. A
`mkfs` that rounds the volume down to a whole number of groups will therefore
write `cg_ncyl = fs_cpg` where `fsck` expects 0, and get
`SUMMARY INFORMATION BAD`. Size to whole cylinders and let the last group be
short, the way `newfs` does.

### 4.3 Cylinder and rotational position

```c
cbtocylno(bno) = bno * fs_nspf / fs_spc
cbtorpos(bno)  = bno * fs_nspf % fs_spc % fs_nsect * NRPOS / fs_nsect
```

with `NRPOS = 8` and `bno` the fragment number relative to `cgbase`. These index
`cg_btot[]` and `cg_b[][]`, which must be kept accurate: `fsck` recomputes and
compares them.

`fs_postbl[cyl][rpos]` in the superblock holds the first block at each
rotational position, and `fs_rotbl[]` chains the rest as deltas, terminated by
0. Both references have `fs_cpc = 1`. HD1, with 8 blocks per cylinder and
positions alternating 0 and 4:

    fs_postbl[0] = [0, -1, -1, -1, 1, -1, -1, -1]
    fs_rotbl     = [2, 2, 2, 2, 2, 2, 0, 0]

These are allocation hints. `fsck` does not check them.

---

## 5. Inodes

128 bytes, `fs_inopb` = 64 per block. Inode *n* is at:

```c
itod(n) = cgimin(n / fs_ipg) + ((n % fs_ipg) / fs_inopb) * fs_frag
byte    = partition_offset + itod(n) * fs_fsize + (n % fs_inopb) * 128
```

Inode 0 and 1 are reserved and marked in use in group 0; 2 is the root, 3 is
conventionally `lost+found`.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `di_mode` | |
| 2 | 2 | `di_nlink` | |
| 4 | 2 | `di_uid` | **16-bit** |
| 6 | 2 | `di_gid` | **16-bit** |
| 8 | 8 | `di_size` | 64-bit; high word first on big-endian |
| 16 | 4 | `di_atime` | + 4 spare |
| 24 | 4 | `di_mtime` | + 4 spare |
| 32 | 4 | `di_ctime` | + 4 spare |
| 40 | 48 | `di_db[12]` | direct block addresses, in fragments |
| 88 | 12 | `di_ib[3]` | single, double, triple indirect |
| 40 | 60 | `di_symlink` | overlays `di_db`/`di_ib` — see §7 |
| 100 | 4 | `di_flags` | `0x0001` = `IC_FASTLINK` |
| 104 | 4 | `di_blocks` | see below |
| 108 | 4 | `di_gen` | |
| 112 | 16 | `di_spare[4]` | |

`di_blocks` counts **fragments**, not 512-byte sectors. `sys/param.h` defines
`DEV_BSIZE` as 512, but `fs_fsbtodb` is 0, so the unit works out to `fs_fsize`.
HD1's `/toolbox-cd` is 53,200 bytes: six whole blocks plus a four-fragment tail
is 52 fragments, and `di_blocks` reads 52.

A block address of 0 is a hole and reads as zeros.

---

## 6. Block allocation, and the rule that bites

An indirect block is a whole `fs_bsize` block of `fs_nindir` = 2048 big-endian
32-bit fragment addresses. Logical block *n* maps through `di_db[n]` for
n < 12, then single, double and triple indirect.

The rule that matters, straight from `blksize()` in NeXT's `fs.h`:

```c
#define blksize(fs, ip, lbn) \
	(((lbn) >= NDADDR || (ip)->i_size >= ((lbn) + 1) << (fs)->fs_bshift) \
	    ? (fs)->fs_bsize \
	    : (fragroundup(fs, blkoff(fs, (ip)->i_size))))
```

In words: **only one of the twelve direct blocks may be a short, fragmented
tail. Any block at or past `NDADDR` is always a whole block**, however few
bytes of it the file uses.

So a 3,000,000-byte file — 366 blocks, the last holding 1632 bytes — occupies a
full 8-fragment block at the end, not two fragments. Allocating only the two
fragments the size implies hands the other six to the next allocation, and
NeXT's `fsck` reports them as `DUP` blocks in two different inodes. This is also
why so many files on a stock OPENSTEP install look like they hold more
fragments than their size needs: they all have more than twelve blocks.

The corollary for freeing is the same rule in reverse: release
`blksize()`-many fragments for the tail, or leak the difference.

Fragment allocation itself follows 4.3BSD: carve a run out of an already-broken
block if one has enough contiguous free fragments, otherwise break up a whole
free block and return the remainder to the free map. Every allocation and free
has to keep five things in step, or `fsck` will notice:

1. `cg_free` — the fragment bitmap
2. `cg_frsum[]` — counts of free runs of each length, whole free blocks excluded
3. `cg_btot[]` and `cg_b[][]` — free whole blocks by cylinder and rotational position
4. `cg_cs` and the `fs_cs` array at `fs_csaddr`
5. `fs_cstotal` in the superblock

Set `fs_fmod` while modifying, and leave `fs_state` at 1.

---

## 7. Directories

Directory data is a run of **1024-byte blocks** (`DIRBLKSIZ`). Within a block
the entries' `d_reclen` values tile it exactly, the last entry's `d_reclen`
running to the end of the block. **No entry may straddle a block boundary.** A
directory's size is always a multiple of 1024.

`DIRBLKSIZ` is `DEV_BSIZE`, which works out as `fs_fsize >> fs_fsbtodb`. That
is 1024 on every NeXT volume, whose `fs_fsbtodb` is 0, and it is worth deriving
rather than hardcoding: A/UX uses the same filesystem with 512-byte blocks
(§12.2).

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `d_ino` — 0 marks a free slot |
| 4 | 2 | `d_reclen` |
| 6 | 2 | `d_namlen` |
| 8 | … | `d_name`, NUL-padded |

    DIRSIZ(namlen) = 8 + ((namlen + 1 + 3) & ~3)

`d_namlen` is a **16-bit** field. There is no `d_type` byte. On a big-endian
volume the two readings are indistinguishable, because a 4.4BSD `d_type` would
be 0 and the length fits in the low byte — which is why misreading this is easy
to get away with until a little-endian volume turns up.

Deleting an entry means either zeroing `d_ino` (if it is first in its block) or
extending the previous entry's `d_reclen` to swallow it. Inserting means finding
a slot whose `d_reclen` minus its real `DIRSIZ` leaves enough room, splitting
it, or appending a fresh 1024-byte block.

---

## 8. Symbolic links

NeXT has **fast symlinks**, unlike stock 4.3BSD. If `di_flags & IC_FASTLINK`
(0x0001), the target string is stored inline in the 60 bytes at offset 40 —
overlaying `di_db` and `di_ib` — and `di_size` is its length. `di_blocks` is 0
and no data block exists.

`MAX_FASTLINK_SIZE` is `(NDADDR + NIADDR) * sizeof(daddr_t)` = 60. Longer
targets are stored as ordinary file data.

On a stock OPENSTEP install essentially every symlink is a fast one:
`/usr/include` → `../NextDeveloper/Headers` lives entirely in the inode.

---

## 9. What `fsck` checks

Useful to know, because it is the acceptance test that matters. Behaviour
observed from NeXT's `/usr/etc/fsck` on OPENSTEP 4.2, matching the 4.3BSD
sources:

- **Phase 1** walks every used inode's block map, claiming fragments and using
  `blksize()` for the tail. Two inodes claiming a fragment gives `DUP`. It
  compares the claimed total against `di_blocks` and reports
  `INCORRECT BLOCK COUNT`.
- **Phase 2** resolves the names of anything phase 1 flagged.
- **Phase 3** checks connectivity from the root; **phase 4** checks link counts.
- **Phase 5** rebuilds each cylinder group and compares. A mismatch in the
  bitmaps is `BLK(S) MISSING IN BIT MAPS`; a mismatch in the header — the
  counters, `cg_frsum`, `cg_btot`, `cg_b`, and notably `cg_ncyl` — is
  `SUMMARY INFORMATION BAD`. It then compares the summed totals against
  `fs_cstotal`.

Exit status 0 means clean; 8 means it wanted to change something.

---

## 10. Variants

- **m68k NeXT hardware** — big-endian, 1024-byte sectors. Everything above.
  Both reference volumes. Linux calls this `ufstype=nextstep`.
- **NeXTSTEP/OPENSTEP for Intel** — the same structures little-endian. Detect by
  reading the superblock magic both ways. The disk label stays big-endian.
  *This tool handles it but it has not been tested against a real Intel volume.*
- **CD-ROM** — `fs_fsize` 2048, and Linux has a separate `ufstype=nextstep-cd`
  for it. The structures are otherwise the same. *Untested here.*
- **A/UX** — the same filesystem on a Macintosh disk, with an Apple Partition
  Map, 512-byte directory blocks and Macintosh metadata in the spare inode
  fields. §12.
- **SunOS** — the same inodes and directory entries, and a different cylinder
  group. §13.
- Linux's `ufstype=openstep` selects the 4.4BSD directory, inode and cylinder
  group layouts. Neither reference OPENSTEP volume uses them: HD00 is old-format
  throughout, and `ufstype=nextstep` is the correct choice for it.

---

## 11. Why existing tools do not work

| Tool | Why not |
|---|---|
| Linux `ufs` | Forces `ro` for `nextstep`, `nextstep-cd`, `openstep` and `old` in `fs/ufs/super.c`. Even with `CONFIG_UFS_FS_WRITE`, `ufs_put_cylinder()` writes the rotors at modern-`cg` offsets, which land inside an old group's `cg_frsum`. |
| FreeBSD/NetBSD `newfs`, `fsck_ffs`, `makefs` | Target FFSv1-dynamic and UFS2: modern cylinder group, 256-byte inodes, 32-bit uids, `d_type` in directory entries. No support for the static `cg`. |
| `fuse-ufs`, UFS2Tool | UFS2, same story. |
| Previous's `ditool` | Reads this format correctly, and is a good cross-check, but is read-only: 262 lines covering inode reads, `bmap`, `readlink`, file reads and directory listing. No allocator, no cylinder group code, no writes. |

---

## 12. A/UX

Apple's UNIX puts the same filesystem on a Macintosh disk: `fs_magic`
`0x00011954`, big-endian, superblock at partition + 8192, and every `struct
fs`, `struct icommon` and directory entry field at the offset §3, §5 and §7
give. What differs is the packaging around it and the Macintosh metadata A/UX
keeps in fields 4.3BSD left spare.

Everything below was read off one reference volume, `AUX_3_1_1GB.dsk`, an A/UX
3.1.1 install. Where something is inferred rather than confirmed it says so.

### 12.1 Finding the filesystem

There is no NeXT label. Block 0 is Apple's driver descriptor (`ER`), and from
block 1 there is an array of one-block partition entries (`PM`), each carrying
the count of entries in the whole map:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `pmSig`, `"PM"` |
| 4 | 4 | `pmMapBlkCnt`, entries in the map |
| 8 | 4 | `pmPyPartStart`, first block |
| 12 | 4 | `pmPartBlkCnt`, length in blocks |
| 16 | 32 | `pmPartName` |
| 48 | 32 | `pmParType` |

A UNIX slice is typed `Apple_UNIX_SVR2`, and a disk normally has three: the
root, swap, and a small "Eschatology" crash-recovery area. Only one of them
holds a filesystem, so picking the largest one whose superblock magic checks
out is what finds the root.

Do not fall back to scanning for the magic on this disk. There is a second
`0x00011954` 4096 bytes before the real superblock, inside the root slice's
boot area, and a scan finds that one first and lands the partition offset 4096
bytes low.

### 12.2 Directory blocks are 512 bytes

`DIRBLKSIZ` is `DEV_BSIZE`, which the geometry gives up as
`fs_fsize >> fs_fsbtodb`: 1024 on NeXT hardware (§7), 512 on A/UX, whose
`fs_fsbtodb` is 1 for its 512-byte sectors. The tiling rule is unchanged, so
the boundary a `d_reclen` must stop at is half as far apart. Reading an A/UX
volume as if its directories were 1024-byte blocks misses it: every entry
still chains correctly, and the damage only appears on a write.

`di_blocks` follows the same unit, counting 512-byte sectors rather than
fragments. `nfrags << fs_fsbtodb` covers both.

### 12.3 The spare inode fields

A/UX documents these itself, in two headers that ship on the volume:
`/usr/include/sys/stat.h` and `/usr/include/sys/xstat.h`. `stat.h` defines
`XINFOSIZ` as 9 and gives `struct xstat` a `long st_xinfo[XINFOSIZ]`;
`xstat.h` overlays that array with a union, one shape for a file and another
for a directory:

```c
union xstat_finfo {
	struct {
		time_t	fdMdDat;	dword	fdType;		dword	fdCreator;
		Point	fdLocation;	word	fdFldr;		word	fdFlags;
		byte	fdScript;	byte	fdXFlags;	finfo_flags vf_flags;
		time_t	fdCrDat;	dword	fdLen;		dword	fdRLen;
	} fl;
	struct {
		CNID	dirID;		word	frView;		uword	frFlags;
		Rect	frRect;		Point	frLocation;	Point	frScroll;
		time_t	frMdDat;	byte	frScript;	byte	frXFlags;
		dword	unused;		word	frNmFls;
	} dr;
	long	size[XINFOSIZ];
};
```

Nine longs, and the inode has exactly nine spare ones: the high half of the
quad size, the three timestamp spares, `di_flags`, and `di_spare[4]`.

| Offset | 4.3BSD | A/UX on a file | A/UX on a directory |
|---|---|---|---|
| 8 | `di_size`, high half | `fdRLen`, resource fork length | `frNmFls`, the valence |
| 20 | `di_atspare` | `fdType`, e.g. `TEXT`, `BIN ` | — |
| 28 | `di_mtspare` | `fdCreator`, e.g. `A/UX`, `MACS` | — |
| 36 | `di_ctspare` | `fdLocation`, the icon position | — |
| 100 | `di_flags` | `fdMdDat`, Finder modification date | `dirID` |
| 112 | `di_spare[0]` | `fdFldr` and `fdFlags` | — |
| 116 | `di_spare[1]` | `fdScript`, `fdXFlags`, `vf_flags` | — |
| 120 | `di_spare[2]` | `fdCrDat`, creation date | `frMdDat` |
| 124 | `di_spare[3]` | `fdLen`, data fork length | zero |

Note that `ufs/inode.h` on the same volume still calls `ic_spare[4]`
"reserved, currently unused". The UFS kernel does not know about any of this;
the Finder layer above it writes the words.

Every entry above was checked against the reference volume. `fdFlags` at 114
decodes as Finder flags on every file that has any: `kHasBundle |
kHasBeenInited` on 26 applications, bare `kHasBeenInited` on documents, and
`kIsAlias | kHasBeenInited` on the one alias. `fdMdDat` equals `di_mtime` on
180 of the 183 files that carry a date, and `fdCrDat` is at or before it on
the same 180. All 44 directories with a `dirID` have zero at 124 and
`frMdDat` equal to `di_mtime`.

**Valence** is a directory's entry count, not counting `.` and `..`. `/bin`
holds 144 entries and its high half reads 144. Read as the top of a 64-bit
size it makes every directory look like gigabytes, which is the first thing to
go wrong on an A/UX volume. A/UX leaves it zero on directories its Finder has
never been shown, so zero means unknown rather than empty.

**The fork lengths.** A file with a resource fork is stored as AppleSingle:
the UNIX file holds a header, the fork lengths and both forks, so `di_size`
is the whole wrapper and `fdLen` is only the data fork inside it. A file with
no resource fork is stored plainly and `fdLen` equals `di_size`. That is what
makes the two distinguishable from the inode alone, and it is why `fdLen` is
only followed when it already agreed with the size.

The alias on the reference volume proves the pair. Its `di_size` is 1529, its
`fdLen` is 0 and its `fdRLen` is 535, and the AppleSingle header inside it
reads:

```
magic 0x00051600  home 'Macintosh'  entries 2
   Finder info    id 9  offset 224  length  32
   resource fork  id 2  offset 512  length 535
```

No data fork entry at all, and a 535-byte resource fork. Checked across the
whole volume, all 182 files that carry Finder data agree, once the `%name`
AppleDouble sidecars are read the way A/UX means them: a sidecar holds the
Finder information of its partner, so `%setfile` has `fdLen` 109764, which is
the length of `setfile`.

**`di_flags` is not `di_flags`.** The root directory reads 2, which is the
Macintosh root directory ID, and the next directories created read 390, 391,
392 in order. A plain file has `fdMdDat` there instead. Two things follow:
`IC_FASTLINK` cannot be read out of this word -- real directories have the
bit set, and honouring it would put a symlink target over their block list --
and A/UX has no fast symlink at all. Every symlink on the reference volume
stores its target in a data block, whatever its length.

### 12.4 No clean marker

`fs_state` is NeXT's own use of a byte 4.3BSD leaves alone, and A/UX leaves it
zero. There is nothing to check before mounting and nothing worth writing to
record one. `fs_cstotal` is stale on the reference volume, still holding the
two directories `newfs` left behind: A/UX appears to write it back only on
unmount. The cylinder group summaries are the truth.

## 13. SunOS

SunOS is not A/UX, but it turns up in the same breath and is worth writing
down. A SunOS 4.1.1 sun3 miniroot reads with no changes at all: same
`fs_magic`, big-endian, `struct fs` and `struct icommon` at the same offsets,
16-bit uids, `d_namlen` a 16-bit field, symlinks in a data block. `fs_fsize`
1024 with `fs_fsbtodb` 1 puts its directories on 512-byte blocks, which the
rule in §7 already gives.

The cylinder group is where it parts company. SunOS writes the **dynamic**
layout, `fs_postblformat` = `FS_DYNAMICPOSTBLFMT`:

| | 4.2BSD (NeXT, A/UX) | dynamic (SunOS and later) |
|---|---|---|
| `cg_magic` | 980 | 4 |
| block totals | 84, fixed | at `cg_btotoff` |
| rotation positions | 212, fixed | at `cg_boff` |
| inodes used | 724, fixed | at `cg_iusedoff` |
| free fragments | 984, fixed | at `cg_freeoff` |

`fs_postblformat` sits at superblock offset 1356, but only in the layout that
has it: in the old one those bytes are rotation table data and could hold
anything. Read the first cylinder group and see which offset the magic number
is at. That is the reliable test, it is what this tool does, and it is what
SunOS's own `cg_chkmagic` does, which accepts either.

The four offsets are read out of each group rather than the superblock,
because that is where they live. A group states them in this order and they
must run in it: `cg_btotoff` (84), `cg_boff` (88), `cg_iusedoff` (92),
`cg_freeoff` (96), `cg_nextfreeoff` (100), the last being where the maps end.
The block-total array holds `(cg_boff - cg_btotoff) / 4` cylinders and the
rotation table `fs_nrpos` entries for each of them, so both are sized to the
group instead of to the 4.2BSD maxima of 32 and 8.

### 13.1 The rotational position of a block

4.3BSD divides a track evenly:

```
	rpos = bno * NSPF % fs_spc % fs_nsect * NRPOS / fs_nsect
```

SunOS folds in three fields NeXT does not have, at superblock offsets 132
(`fs_npsect`, sectors per track including spares), 136 (`fs_interleave`) and
140 (`fs_trackskew`):

```
	rpos = (bno * NSPF % fs_spc / fs_nsect * fs_trackskew +
	        bno * NSPF % fs_spc % fs_nsect * fs_interleave)
	       % fs_nsect * fs_nrpos / fs_npsect
```

With no skew, no interleave and no spare sectors that is the 4.3BSD formula
again. NeXT keeps its `fs_sparecon` at those same offsets, so the longer form
must only be used on a volume that has the dynamic layout.

### 13.2 The clean flag

Offset 209 is `fs_state` on NeXT and `fs_clean` on SunOS, and they disagree
about which way it counts. NeXT's 1 is clean; Sun's `FSACTIVE` is 0 and means
the volume was in use and may not be. Sun's values are 0 `FSACTIVE`,
1 `FSCLEAN`, 2 `FSSTABLE`.

Sun does not trust that byte on its own. A second word has to hold
`FSOKAY - fs_time`, where `FSOKAY` is `0x7c269d38`, and a volume whose
`fs_time` has moved on since is checked whatever the byte says. The word is at
offset 0 in the 4.2BSD layout, overloading `fs_link` so that an old `fsck`
still works, and at `fs_sparecon[55]`, offset 1336, in the dynamic one.

### 13.3 Timestamps

Each of the three timestamps is a `struct timeval`, not a bare `time_t`. The
word after each second holds its microseconds: offsets 20, 28 and 36, which
4.3BSD calls `ic_atspare`, `ic_mtspare` and `ic_ctspare` and A/UX fills with
Finder data. Setting a time to a new second and leaving the old fraction
behind produces a timestamp that never existed, so the fraction is cleared
whenever the second changes.

### 13.4 What SunOS leaves alone

`di_flags` (100) and the four words at 112-127 are zero on every inode of the
reference volume. SunOS 4.1's `struct icommon` does name those four,
`ic_delaylen`, `ic_delayoff`, `ic_nextrio` and `ic_writes`, but they are
in-core scratch that the kernel clears on the way to disk. So a SunOS volume
carries no metadata of its own in the fields A/UX uses, and nothing has to be
preserved through a write beyond what §12 already says.

Solaris 2 is a different filesystem in this respect and is not covered here.

---

## 14. Sources

- NeXT's own headers, in `docs/next-headers/`: `ufs/fs.h`, `ufs/inode.h`,
  `ufs/fsdir.h`, `dev/disk_label.h`, `sys/disktab.h`, `sys/param.h`. Extracted
  from an OPENSTEP 4.2 image.
- The label checksum matches NetBSD's `sys/arch/next68k/next68k/disksubr.c`.
- Linux `fs/ufs/` for the flavour flags and a second opinion on the old layouts.
- Previous's `src/ditool/` for an independent reader to check against.
- The two reference volumes, and NeXTSTEP's `fsck` running in Previous.
- For §12, `AUX_3_1_1GB.dsk`, an A/UX 3.1.1 install, plus Apple's partition map
  layout as `Inside Macintosh: Devices` documents it. The spare inode fields
  in §12.3 come from A/UX's own `sys/xstat.h`, `sys/stat.h`, `ufs/inode.h`,
  `mac/files.h` and `mac/asd.h`, which ship in `/usr/include` on that very
  volume and are copied to `local-scratch/src/aux-headers/`.
- For §13, the SunOS 4.1.1 sun3 install miniroot, and SunOS 4.1.4's own
  `sys/ufs/fs.h`, `sys/ufs/inode.h` and `etc/fsck/`, from the source tree in
  archive.org item `titor-special_202112`. Everything §13 states about the
  dynamic cylinder group, `cbtorpos`, the clean flag and the timestamps is
  from those headers, not inferred from the volume.
