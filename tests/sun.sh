#!/usr/bin/env bash
# Read and write a SunOS volume: the dynamic cylinder group, and the clean
# flag SunOS keeps where NeXT keeps fs_state.
#
# SunOS uses the same inodes and directory entries as NeXT. What differs is
# the cylinder group, which carries the offsets of its own four maps instead
# of having them at fixed places. fsck rebuilds every free map and summary
# from the inodes, so running it after a write is what proves the layout is
# being read right.
#
# The image is the SunOS 4.1.1 sun3 install miniroot, 7MB, a bare filesystem
# with no partition table. It is item titor-special_202112 on archive.org,
# under UNIX-Source-Code/SUN/www.sun3zoo.de/sun3arc/BootTapes/Sun3/. Put it in
# local-scratch/images/ or pass a path.
set -u

NEXTUFS=${NEXTUFS:-./build/nextufs}
SRC=${1:-${SUN_IMAGE:-local-scratch/images/miniroot_sun3}}
W=$(mktemp -d "${TMPDIR:-/tmp}/nextufs-sun.XXXXXX")
IMG=$W/img
fail=0

cleanup() { rm -rf "$W"; }
trap cleanup EXIT

[ -r "$SRC" ] || { echo "skip: no SunOS image at $SRC"; exit 77; }

ok()   { echo "ok    $1"; }
bad()  { echo "FAIL  $1"; fail=1; }
want() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

problems() {			# how many problems fsck reports
	"$NEXTUFS" "$IMG" fsck 2>&1 | sed -n 's/^\([0-9]*\) problems* found.*/\1/p'
}

check() {
	local what=$1 n
	n=$(problems)
	if [ "$n" = 0 ]; then
		ok "$what"
	else
		bad "$what ($n problems)"
		"$NEXTUFS" "$IMG" fsck 2>&1 | sed 's/^/      /'
	fi
}

info=$("$NEXTUFS" "$SRC" info)
grep -q '^variant   SunOS' <<<"$info" && ok "the cylinder group layout is recognised" \
	|| bad "the cylinder group layout is recognised"
want "the volume is big-endian" \
	"$(sed -n 's/^byteorder //p' <<<"$info")" "big-endian"
want "the mount point in the superblock" \
	"$(sed -n 's/^mounted   //p' <<<"$info")" "/miniroot"

# The miniroot was written by an installer and never unmounted, so SunOS
# would check it too. fs_clean and fs_state must both be read to see that.
grep -q '^state .*fs_clean 0 (not clean)' <<<"$info" \
	&& ok "the clean flag is read the way SunOS writes it" \
	|| bad "the clean flag is read the way SunOS writes it"

# --- reading -------------------------------------------------------------
# 1024-byte fragments with fs_fsbtodb 1 puts directories on 512-byte blocks,
# the same as A/UX. Reading them as 1024 would run entries off the end.
want "the root directory's size" \
	"$("$NEXTUFS" "$SRC" ls -l / | awk '$NF=="."{print $5}')" "512"
want "a directory reads back" \
	"$("$NEXTUFS" "$SRC" ls /bin | grep -c .)" "44"
want "a symlink target comes out of its data block" \
	"$("$NEXTUFS" "$SRC" readlink /lib)" "usr/lib"
want "a small file reads back" \
	"$("$NEXTUFS" "$SRC" cat /.MINIROOT)" "Sun Oct 14 16:20:16 PDT 1990"

# The kernel is 664KB, so it exercises the indirect blocks.
"$NEXTUFS" "$SRC" get /vmunix "$W/vmunix" || bad "get /vmunix"
want "a file with indirect blocks comes out whole" \
	"$(stat -c %s "$W/vmunix" 2>/dev/null)" "679920"
want "and it starts with a 68k a.out header" \
	"$(head -c 4 "$W/vmunix" | xxd -p)" "00020107"

want "the whole tree walks" "$("$NEXTUFS" "$SRC" find / | grep -c .)" "1026"

# SunOS stores a whole struct timeval, so each timestamp is followed by its
# microseconds. NeXT leaves those words spare, so nothing read them before.
want "timestamps carry their microseconds" \
	"$("$NEXTUFS" "$SRC" stat /vmunix | sed -n 's/^times   //p')" \
	"a 655946420.040006 m 655946420.140003 c 655946420.140003"

# --- writing -------------------------------------------------------------
# fsck recounts the free maps, the per-cylinder block totals and the
# rotational-position table from the inodes alone. Coming out at zero after a
# write means every one of those was found and updated where SunOS keeps it.
cp "$SRC" "$IMG"
"$NEXTUFS" "$IMG" fsck -y >/dev/null 2>&1	# clear the installer's dirty flag
check "the volume checks out before anything is written"

head -c 30000 /dev/urandom > "$W/blob"
"$NEXTUFS" "$IMG" put "$W/blob" /etc/blob || bad "put"
check "after writing a file"
# A rewritten timestamp must not keep the fraction of the second it replaced.
frac=$("$NEXTUFS" "$IMG" stat /etc/blob | sed -n 's/^times .*m [0-9]*\.\([0-9]*\) .*/\1/p')
want "a new timestamp gets a fresh fraction, not the old one" "$frac" "000000"

"$NEXTUFS" "$IMG" get /etc/blob "$W/back" || bad "get"
cmp -s "$W/blob" "$W/back" && ok "the file reads back identical" \
	|| bad "the file reads back identical"

# Enough files to spill into a second cylinder group, which is where a wrong
# map offset would show up: each group states where its own maps begin.
"$NEXTUFS" "$IMG" mkdir /etc/manysun || bad "mkdir"
for i in $(seq 1 40); do
	"$NEXTUFS" "$IMG" put "$W/blob" "/etc/manysun/entry-number-$i" || {
		bad "put entry $i"; break; }
done
check "after 40 more files"

"$NEXTUFS" "$IMG" ln -s /etc/blob /etc/blob.link || bad "symlink"
want "the symlink reads back" "$("$NEXTUFS" "$IMG" readlink /etc/blob.link)" \
	"/etc/blob"
"$NEXTUFS" "$IMG" mv /etc/blob /etc/blob2 || bad "mv"
check "after a symlink and a rename"

for i in $(seq 1 40); do
	"$NEXTUFS" "$IMG" rm "/etc/manysun/entry-number-$i" || {
		bad "rm entry $i"; break; }
done
"$NEXTUFS" "$IMG" rmdir /etc/manysun || bad "rmdir"
"$NEXTUFS" "$IMG" rm /etc/blob.link || bad "rm symlink"
"$NEXTUFS" "$IMG" rm /etc/blob2 || bad "rm"
check "after removing everything again"
want "free space is back where it started" \
	"$("$NEXTUFS" "$IMG" info | sed -n 's/^free      //p')" \
	"$("$NEXTUFS" "$SRC" info | sed -n 's/^free      //p')"

# --- the clean flag round-trips ------------------------------------------
"$NEXTUFS" "$IMG" fsck -y >/dev/null 2>&1
grep -q '^state .*fs_clean 1 (clean)' <<<"$("$NEXTUFS" "$IMG" info)" \
	&& ok "fsck -y marks it clean the way SunOS would" \
	|| bad "fsck -y marks it clean the way SunOS would"

# --- nothing that was already there moved --------------------------------
"$NEXTUFS" "$IMG" get /vmunix "$W/vmunix2" || bad "get /vmunix again"
cmp -s "$W/vmunix" "$W/vmunix2" && ok "an untouched file is still byte-identical" \
	|| bad "an untouched file is still byte-identical"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
