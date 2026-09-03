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

## 12. Sources

- NeXT's own headers, in `docs/next-headers/`: `ufs/fs.h`, `ufs/inode.h`,
  `ufs/fsdir.h`, `dev/disk_label.h`, `sys/disktab.h`, `sys/param.h`. Extracted
  from an OPENSTEP 4.2 image.
- The label checksum matches NetBSD's `sys/arch/next68k/next68k/disksubr.c`.
- Linux `fs/ufs/` for the flavour flags and a second opinion on the old layouts.
- Previous's `src/ditool/` for an independent reader to check against.
- The two reference volumes, and NeXTSTEP's `fsck` running in Previous.
