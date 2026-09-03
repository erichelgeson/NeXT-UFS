# nextufs

Read and write NeXTSTEP/OPENSTEP disks from Linux, macOS or anywhere else with
a C compiler. Copy files in and out of a `.hda` image, edit a NeXT volume in
place, create a new one from nothing, and check one for damage.

Linux can read these disks but cannot write them: the kernel's `ufs` driver
forces `ro` for `ufstype=nextstep`, `nextstep-cd` and `openstep`, and no BSD
userland tool understands the format either. NeXT uses the original 4.3BSD
filesystem — static cylinder groups, 128-byte inodes, 16-bit uids — which
FreeBSD and NetBSD dropped long ago.

## Build

    make

No dependencies. The binary lands in `build/nextufs`.

## Use

    nextufs <image> <command> [args]        # add -p b to pick a partition

    info                    the disk label and filesystem geometry
    ls [path]               list a directory
    cat <path>              write a file to stdout
    get [-r] <path> <host>  copy a file or directory tree out
    put [-r] <host> <path>  copy a file or directory tree in
    find [path]             list every path underneath
    stat <path>             one inode in detail
    readlink <path>         a symlink's target

    mkdir <path>            create a directory
    rmdir <path>            remove an empty directory
    rm <path>               remove a file
    mv <from> <to>          rename or move
    ln -s <target> <path>   create a symbolic link
    chmod <mode> <path>     change permissions, octal
    chown <uid>[:<gid>] <p> change owner

    mkfs <MB> [label]       create a labelled, empty NeXT volume
    fsck [-y]               check the filesystem; -y repairs it

Some examples:

    nextufs HD1.hda put -r ~/src/myproject /source/myproject
    nextufs HD1.hda get -r /source ~/recovered
    nextufs new.hda mkfs 200 "Work Disk"
    nextufs HD1.hda fsck

Take a copy before writing to a disk you care about, and run `fsck` afterwards.

## What it understands

A NeXT disk starts with a `dlV3` label at sector 0 giving the geometry, the
160-sector front porch and up to eight partitions. The filesystem inside is
4.3BSD FFS, big-endian on m68k hardware, with 8KB blocks and 1KB fragments.
Little-endian volumes (NeXTSTEP for Intel) are detected and handled too.

Structure layouts came from NeXT's own headers, which are in
`docs/next-headers/` — extracted from an OPENSTEP 4.2 disk image by an earlier
version of this tool.

## Verification

`tests/stress.sh <image>` writes files of every interesting size, fills a
directory past one block, makes and removes trees, and checks consistency at
each step. It finishes by confirming free space is exactly what it started
with and that nothing already on the disk changed.

Beyond that, the tool has been checked against three independent references:

- Every file read out of two real NeXT volumes matches what Previous's
  `ditool` extracts, byte for byte.
- `nextufs fsck` reports no problems on those volumes untouched.
- **NeXTSTEP's own `fsck`, run inside the Previous emulator, exits 0** on a
  real volume written to by this tool and on a volume `mkfs` built from
  nothing.

That last one is the test that matters, and it is what caught the two real
bugs this code had: a tail block must be a whole block once a file grows past
the twelve direct blocks (NeXT's `blksize()` macro), and the last cylinder
group's `cg_ncyl` holds `fs_ncyl % fs_cpg`, which is what `fsck` rebuilds and
compares.

## Limitations

- `fsck` repairs accounting: free maps, summary counters and link counts. It
  reports duplicate or out-of-range blocks rather than trying to fix them.
- Truncation is to zero only; a file is rewritten rather than shortened.
- Hard links can be read but not created.
