#!/usr/bin/env bash
# Read and write an A/UX volume: an Apple Partition Map, 512-byte directory
# blocks, and the Macintosh metadata A/UX keeps in the spare inode fields.
#
# Needs a real A/UX disk image, which is far too big to keep in the repo, so
# the run exits 77 (skipped) when it cannot find one. Pass a path to override.
set -u

NEXTUFS=${NEXTUFS:-./build/nextufs}
SRC=${1:-${AUX_IMAGE:-$HOME/Downloads/AUX_3_1_1GB_Use_In_Shoebill/AUX_3_1_1GB.dsk}}
W=$(mktemp -d "${TMPDIR:-/tmp}/nextufs-aux.XXXXXX")
IMG=$W/img.dsk
fail=0

cleanup() { rm -rf "$W"; }
trap cleanup EXIT

[ -r "$SRC" ] || { echo "skip: no A/UX image at $SRC"; exit 77; }

ok()   { echo "ok    $1"; }
bad()  { echo "FAIL  $1"; fail=1; }
want() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

problems() {			# how many problems fsck reports
	"$NEXTUFS" "$IMG" fsck 2>&1 | sed -n 's/^\([0-9]*\) problems* found.*/\1/p'
}

# A real A/UX disk is not a clean volume -- this one was snapshotted while
# mounted -- so what matters is that nothing we do makes it any worse.
check() {
	local what=$1 n
	n=$(problems)
	if [ "$n" -gt "$BASE" ]; then
		bad "$what ($n problems, baseline $BASE)"
		"$NEXTUFS" "$IMG" fsck 2>&1 | sed 's/^/      /'
	else
		ok "$what"
	fi
}

# --- read the whole disk, not a carved-out slice ------------------------
info=$("$NEXTUFS" "$SRC" info)
grep -q 'Apple_UNIX_SVR2' <<<"$info" && ok "info lists the Apple Partition Map" \
	|| bad "info lists the Apple Partition Map"
grep -q '^variant   A/UX' <<<"$info" && ok "the volume is recognised as A/UX" \
	|| bad "the volume is recognised as A/UX"
want "the root slice is chosen by default" \
	"$(sed -n 's/^using     partition \([0-9]*\).*/\1/p' <<<"$info")" "5"

# The valence lives in the high half of di_size, so a directory read without
# it comes out gigabytes long.
want "a directory's size is its real size" \
	"$("$NEXTUFS" "$SRC" ls -l / | awk '$NF=="lost+found"{print $5}')" "8192"
want "a directory's valence is read" \
	"$("$NEXTUFS" "$SRC" stat /bin | awk '$1=="valence"{print $2}')" "144"
want "a file's Finder type and creator are read" \
	"$("$NEXTUFS" "$SRC" stat /newunix | sed -n 's/^finder  //p')" \
	'type "BIN " creator "A/UX"'
want "a symlink target comes out of its data block" \
	"$("$NEXTUFS" "$SRC" readlink /etc/newfs)" "/etc/fs/ufs/newfs"
want "-p selects a slice by name" \
	"$("$NEXTUFS" -p 'UNIX Root&Usr slice 0' "$SRC" ls / | wc -l)" \
	"$("$NEXTUFS" "$SRC" ls / | wc -l)"

# --- a slice carved out on its own, with no partition map ---------------
# Without the map, A/UX has to be recognised from the root inode. The valence
# is the obvious mark but A/UX leaves it zero on a volume its Finder has not
# opened, so the Macintosh directory ID has to carry it: the root is always 2.
start=$(sed -n 's/^ *5: start *\([0-9]*\).*/\1/p' <<<"$info")
size=$(sed -n 's/^ *5: start *[0-9]* *size *\([0-9]*\).*/\1/p' <<<"$info")
dd if="$SRC" of="$W/bare" bs=512 skip="$start" count="$size" status=none
want "a bare slice is still recognised as A/UX" \
	"$("$NEXTUFS" "$W/bare" info | sed -n 's/^variant   \(A.UX\).*/\1/p')" "A/UX"
# Zero the root's valence, which is what a freshly installed volume looks like.
printf '\0\0\0\0' | dd of="$W/bare" bs=1 seek=$(( $("$NEXTUFS" "$W/bare" info |
	sed -n 's/^layout    sblkno [0-9]* cblkno [0-9]* iblkno \([0-9]*\).*/\1/p') *
	1024 + 2 * 128 + 8 )) conv=notrunc status=none
want "and still recognised with the valence cleared" \
	"$("$NEXTUFS" "$W/bare" info | sed -n 's/^variant   \(A.UX\).*/\1/p')" "A/UX"
want "the Macintosh directory ID of the root is fsRtDirID" \
	"$("$NEXTUFS" "$W/bare" stat / | awk '$1=="mac"{print $3}')" "2"

# --- now write to a copy ------------------------------------------------
cp "$SRC" "$IMG"
BASE=$(problems)
echo "      baseline: $BASE pre-existing problems"

# A/UX lets a directory's stored valence drift -- /etc really holds more
# entries than its own count claims -- so what a write must produce is the
# true count, not the stale one plus one.
before=$(( $("$NEXTUFS" "$IMG" ls /etc | grep -c .) - 2 ))
head -c 30000 /dev/urandom > "$W/blob"
"$NEXTUFS" "$IMG" put "$W/blob" /etc/blob || bad "put"
check "after writing a file"
want "the parent's valence counts what it now holds" \
	"$("$NEXTUFS" "$IMG" stat /etc | awk '$1=="valence"{print $2}')" \
	"$((before + 1))"

"$NEXTUFS" "$IMG" get /etc/blob "$W/back" || bad "get"
cmp -s "$W/blob" "$W/back" && ok "the file reads back identical" \
	|| bad "the file reads back identical"

# Enough entries to spill over several directory blocks, which is where a
# wrong DIRBLKSIZ shows up: fsck checks that entries tile every block.
"$NEXTUFS" "$IMG" mkdir /etc/manyaux || bad "mkdir"
for i in $(seq 1 120); do
	"$NEXTUFS" "$IMG" put "$W/blob" "/etc/manyaux/entry-number-$i" || {
		bad "put entry $i"; break; }
done
check "after 120 entries in one directory"
want "the new directory's valence" \
	"$("$NEXTUFS" "$IMG" stat /etc/manyaux | awk '$1=="valence"{print $2}')" \
	"120"

"$NEXTUFS" "$IMG" ln -s /etc/blob /etc/blob.link || bad "symlink"
want "an A/UX symlink is not stored inline" \
	"$("$NEXTUFS" "$IMG" stat /etc/blob.link | awk '$1=="blocks"{print $2}')" "2"
want "the symlink reads back" "$("$NEXTUFS" "$IMG" readlink /etc/blob.link)" \
	"/etc/blob"
check "after a symlink"

"$NEXTUFS" "$IMG" mv /etc/blob /etc/blob2 || bad "mv"
check "after a rename"

for i in $(seq 1 120); do
	"$NEXTUFS" "$IMG" rm "/etc/manyaux/entry-number-$i" || {
		bad "rm entry $i"; break; }
done
"$NEXTUFS" "$IMG" rmdir /etc/manyaux || bad "rmdir"
"$NEXTUFS" "$IMG" rm /etc/blob.link || bad "rm symlink"
"$NEXTUFS" "$IMG" rm /etc/blob2 || bad "rm"
check "after removing everything again"
want "the parent's valence is back to the entries it holds" \
	"$("$NEXTUFS" "$IMG" stat /etc | awk '$1=="valence"{print $2}')" "$before"

# --- the Finder's own copies of the length and the date -----------------
# A/UX keeps fdLen and fdMdDat in the spare inode longs and the Finder shows
# those, not what UNIX holds. See NeXT-UFS-Spec.md 12.3.
fork() { "$NEXTUFS" "$IMG" stat "$1" | sed -n 's/^forks   data \([0-9]*\),.*/\1/p'; }
mdat() { "$NEXTUFS" "$IMG" stat "$1" | sed -n 's/^mac date created [0-9]*, modified //p'; }

# These follow the file only where the file keeps its inode, so the test
# changes one in place. "put" replaces a file outright, and a replaced file is
# a new one that the Finder has not seen.
want "a plain file's fdLen starts out as its size" "$(fork /mac/bin/mac32)" "1969"
before=$(mdat /mac/bin/mac32)
"$NEXTUFS" "$IMG" truncate /mac/bin/mac32 1000 || bad "truncate"
want "and follows the file when it changes length" "$(fork /mac/bin/mac32)" "1000"
[ "$(mdat /mac/bin/mac32)" != "$before" ] \
	&& ok "fdMdDat moves with the file" \
	|| bad "fdMdDat moves with the file (still $before)"
check "after changing a file the Finder knows"

# An AppleSingle file: the UNIX file holds a header and both forks, so fdLen
# is rightly smaller than the size and must not be dragged along with it.
want "an AppleSingle file's fdLen is not its size" "$(fork /mac/bin/rez)" "0"
"$NEXTUFS" "$IMG" truncate /mac/bin/rez 5000 || bad "truncate AppleSingle"
want "and is left alone when the file changes" "$(fork /mac/bin/rez)" "0"

# A file the Finder has never seen gets no Finder data invented for it, even
# when it reuses the inode of one that had some.
"$NEXTUFS" "$IMG" rm /mac/bin/startmac || bad "rm a file with Finder data"
head -c 4321 /dev/urandom > "$W/small"
"$NEXTUFS" "$IMG" put "$W/small" /etc/plainfile || bad "put a new file"
want "a new file gets no Finder length" "$(fork /etc/plainfile)" "0"
want "and no Finder date" "$(mdat /etc/plainfile)" ""
want "and no Finder type" \
	"$("$NEXTUFS" "$IMG" stat /etc/plainfile | sed -n 's/^finder  //p')" ""
"$NEXTUFS" "$IMG" rm /etc/plainfile || bad "rm"
check "after writing a file the Finder has never seen"

# --- nothing that was already there moved -------------------------------
"$NEXTUFS" "$SRC" cat /newunix > "$W/a" 2>/dev/null
"$NEXTUFS" "$IMG" cat /newunix > "$W/b" 2>/dev/null
cmp -s "$W/a" "$W/b" && ok "an untouched file is still byte-identical" \
	|| bad "an untouched file is still byte-identical"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
