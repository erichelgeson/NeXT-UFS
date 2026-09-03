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

    nextufs <image> <command> [args]        # -p picks a partition

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
    ln <existing> <path>    create a hard link
    truncate <path> <size>  set a file's length, padding with zeros
    putat <host> <path> <n> write a host file into an existing file at offset n
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

## Mount it

    make fuse
    build/nextufs-fuse HD1.hda /mnt/next
    fusermount3 -u /mnt/next

The image becomes a normal directory: copy files in and out, edit them in
place, run rsync or tar over it. This is the only part that needs a library
off your machine, libfuse3, and plain `make` never looks for it.

    -o ro                          mount read-only
    -o part=b                      pick a partition
    -o force                       mount a volume that was not cleanly unmounted
    -o uid=1000,gid=100,umask=022  show every file as yours

Files keep the owner and mode NeXT gave them, and nothing is checked against
your own user. Add `-o default_permissions` to have the kernel enforce them,
and `-o allow_other` to let other users in (on NixOS that one needs
`programs.fuse.userAllowOther = true`).

`mount` and fstab work too:

    mount -t fuse.nextufs-fuse HD1.hda /mnt/next -o part=a
    /path/HD1.hda  /mnt/next  fuse.nextufs-fuse  part=a,user,noauto  0 0

One program writes an image at a time. The CLI refuses an image that is
mounted, and a second mount of the same image is refused as well.

A mounted volume is marked dirty until it is unmounted, which is what NeXTSTEP
itself does. Kill the mount and the mark stays behind: run
`nextufs HD1.hda fsck -y` before mounting it read-write again, or mount it
with `-o force`. A crash can also leave `.fuse_hiddenXXXX` files behind, which
are safe to delete.

Reading a file does not update its access time.

## What it understands

A NeXT disk starts with a `dlV3` label at sector 0 giving the geometry, the
160-sector front porch and up to eight partitions. The filesystem inside is
4.3BSD FFS, big-endian on m68k hardware, with 8KB blocks and 1KB fragments.
Little-endian volumes (NeXTSTEP for Intel) are detected and handled too.

Structure layouts came from NeXT's own headers, which are in
`docs/next-headers/` — extracted from an OPENSTEP 4.2 disk image by an earlier
version of this tool.

## Also supports

**A/UX.** Apple's UNIX for the Macintosh put the same 4.3BSD filesystem on
disk, so every command above works on an A/UX image. Give it the whole disk:
it reads the Apple Partition Map and takes the root slice. `-p 5` or
`-p 'UNIX Root&Usr slice 0'` picks a different one.

A/UX keeps Macintosh metadata in the nine long words 4.3BSD leaves spare, and
those nine mean one thing on a file and another on a directory. Four of the
differences change how a volume must be read or written:

- Directories are built from 512-byte blocks rather than 1024.
- A directory's entry count lives in the unused top half of its size. Anything
  that reads the size as one 64-bit number reports every directory as
  gigabytes long. `nextufs` keeps the count right as you add and remove
  entries, and `fsck` reports one that has drifted.
- The word NeXT uses for flags is a Macintosh directory ID here, so the
  fast-symlink bit it holds means nothing. A/UX symlinks always store their
  target in a data block.
- There is no clean/dirty marker. A volume is never refused as unclean, and
  mounting one writes nothing to record it.

The Finder keeps its own copy of a file's length and modification date, and it
shows those rather than what UNIX holds. `nextufs` moves both along when it
changes a file, so a file edited here does not turn up in the Finder described
as something it no longer is. A file the Finder has never seen is left alone,
the way A/UX itself fills these in only once it has looked.

`stat` shows what the volume holds for each: the Finder type and creator, the
fork lengths and dates on a file, the entry count and directory ID on a
directory.

A file with a resource fork is stored as AppleSingle, so the UNIX file is a
wrapper holding a header and both forks. `nextufs` reads and writes the
wrapper as the bytes it is and does not take the forks apart.

`mkfs` builds NeXT volumes only.

**SunOS.** SunOS uses the same inodes and directory entries, so every command
above works on a SunOS volume. What differs is the cylinder group. Sun moved to
the dynamic form, where `cg_magic` sits at the front of the group and each
group states where its own four maps begin. `nextufs` tells the two apart by
reading the group, which is how SunOS itself does it, and takes the map
offsets from the group it is working in.

Two smaller differences:

- The byte NeXT uses for `fs_state` is Sun's `fs_clean`, and it counts the
  other way: zero is the volume that needs checking. Sun believes that byte
  only while a second word vouches for it, so `nextufs` reads both and
  `fsck -y` sets both.
- Timestamps are a whole `struct timeval`. Each second is followed by its
  microseconds, in a word NeXT leaves empty. `stat` shows them.

Checked against the SunOS 4.1.1 sun3 install miniroot and against SunOS
4.1.4's own `sys/ufs/fs.h`. Later BSDs and Solaris write the same cylinder
group, so they should work the same way, and no version past SunOS 4 has been
tried. Solaris 2 is the one to be careful with: it moved to a 32-bit uid and
gid kept in fields SunOS 4 leaves empty, and nothing here knows about that.

## Verification

`tests/fuse.sh` mounts an image and drives the driver through the same ground
from the other side, then checks the volume the kernel left behind. It skips
itself where there is no `/dev/fuse`.

`tests/aux.sh <image>` does the same against a real A/UX disk, and adds the
checks that are specific to it: the partition map, the entry count, 512-byte
directory blocks, and the Finder fields, including that a file the Finder has
never seen gets none invented for it. Because a snapshot of a live A/UX disk is
never a clean volume, it holds the tool to leaving the problem count no worse
than it found it. It skips itself when there is no image to run on.

`tests/sun.sh <image>` reads a SunOS volume and writes to a copy, running
`fsck` after every step. That is the check that matters for SunOS: `fsck`
rebuilds every free map, per-cylinder block total and rotational-position
count from the inodes alone, so agreeing with the volume afterwards means each
one was found and updated where SunOS keeps it. It names where to get the
image it wants.

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
- Extended attributes are not supported.
- `mkfs` makes NeXT volumes. There is no A/UX or SunOS equivalent.
- Little-endian volumes are handled but have never been run against a real
  NeXTSTEP for Intel disk.
