#!/usr/bin/env bash
# Recognise a Solaris 2 volume, read it, and refuse to write it.
#
# SVR4 UFS is the same filesystem again, with the same magic and the same
# dynamic cylinder group SunOS 4 uses. What differs is the inode: Solaris
# spent the four words at 112-127 that SunOS leaves empty, on the shadow
# inode holding the ACL and the real 32-bit uid and gid. Writing one of these
# as though those words were spare would destroy an inode's owner, so the
# only safe thing is to know one when we see it.
#
# The image is the Solaris 2.6 5/98 SPARC disk from archive.org item
# solaris265-qemu, converted out of QEMU's format:
#
#	7z e solaris265.7z sparc.qcow2
#	qemu-img convert -O raw sparc.qcow2 solaris26.img
#
# Put the result in local-scratch/images/ or pass a path.
set -u

NEXTUFS=${NEXTUFS:-./build/nextufs}
SRC=${1:-${SOLARIS_IMAGE:-local-scratch/images/solaris26.img}}
W=$(mktemp -d "${TMPDIR:-/tmp}/nextufs-sol.XXXXXX")
fail=0

cleanup() { rm -rf "$W"; }
trap cleanup EXIT

[ -r "$SRC" ] || { echo "skip: no Solaris image at $SRC"; exit 77; }

ok()   { echo "ok    $1"; }
bad()  { echo "FAIL  $1"; fail=1; }
want() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

info=$("$NEXTUFS" "$SRC" info)
grep -q '^variant   Solaris 2' <<<"$info" && ok "the volume is recognised as Solaris" \
	|| bad "the volume is recognised as Solaris"
want "the mount point in the superblock" \
	"$(sed -n 's/^mounted   //p' <<<"$info")" "/"

# Solaris writes the same clean flag SunOS does, vouched for by the same
# fs_state word. This one was shut down cleanly, so it reads FSSTABLE.
grep -q '^state .*fs_clean 2 (clean)' <<<"$info" \
	&& ok "the clean flag is read the way Solaris writes it" \
	|| bad "the clean flag is read the way Solaris writes it"

# --- it must not be written ----------------------------------------------
out=$("$NEXTUFS" "$SRC" mkdir /nope 2>&1)
[ $? != 0 ] && ok "a write is refused" || bad "a write is refused"
grep -q 'SVR4 UFS' <<<"$out" && ok "and says why" || bad "and says why"

# --- but it reads --------------------------------------------------------
want "the root directory lists" "$("$NEXTUFS" "$SRC" ls / | grep -c .)" "29"
want "a symlink target comes out" "$("$NEXTUFS" "$SRC" readlink /bin)" "./usr/bin"
grep -q 'Solaris 2.6' <<<"$("$NEXTUFS" "$SRC" cat /etc/release)" \
	&& ok "a file reads back" || bad "a file reads back"

# 183 cylinder groups of 88 cylinders, against the miniroot's 4 of 16. fsck
# rebuilds every free map and rotational tally from the inodes, so a clean
# run here is the dynamic cylinder group checked against a second geometry.
"$NEXTUFS" "$SRC" fsck > "$W/fsck" 2>&1
grep -q '^0 problems found' "$W/fsck" \
	&& ok "fsck rebuilds every map and finds nothing wrong" \
	|| { bad "fsck rebuilds every map and finds nothing wrong"; sed 's/^/      /' "$W/fsck"; }

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
