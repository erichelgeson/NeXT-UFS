#!/usr/bin/env bash
# Exercise the write path against a scratch copy of a real NeXT volume and
# check consistency after every stage. Any fsck problem fails the run.
set -u

NEXTUFS=${NEXTUFS:-./build/nextufs}
SRC=${1:-"HD1 - BlueSCSIToolboxNeXT.hda"}
W=$(mktemp -d "${TMPDIR:-/tmp}/nextufs-stress.XXXXXX")
IMG=$W/img.hda
fail=0

cleanup() { rm -rf "$W"; }
trap cleanup EXIT

check() {
	local what=$1 out
	out=$("$NEXTUFS" "$IMG" fsck 2>&1)
	if ! grep -q '^0 problems found' <<<"$out"; then
		echo "FAIL  $what"
		sed 's/^/      /' <<<"$out"
		fail=1
	else
		echo "ok    $what"
	fi
}

cp "$SRC" "$IMG"
check "baseline (untouched copy)"

# --- files of every interesting size ------------------------------------
mkdir -p "$W/src"
sizes="0 1 1023 1024 1025 4095 8191 8192 8193 98303 98304 106496 200000 1000000"
for s in $sizes; do
	head -c "$s" /dev/urandom > "$W/src/f$s"
done
# large enough to need a double indirect block (> 12 + 2048 blocks)
head -c 17000000 /dev/urandom > "$W/src/fbig"

for f in "$W"/src/*; do
	"$NEXTUFS" "$IMG" put "$f" "/$(basename "$f")" || { echo "FAIL put $f"; fail=1; }
done
check "after writing $(ls "$W/src" | wc -l) files"

echo "--- content round-trip"
for f in "$W"/src/*; do
	n=$(basename "$f")
	if ! "$NEXTUFS" "$IMG" cat "/$n" | cmp -s - "$f"; then
		echo "FAIL  content differs for $n"
		fail=1
	fi
done
echo "ok    all files read back identical"

# --- truncate, growth and sparse writes ---------------------------------
mkdir -p "$W/mir"
for n in f1 f1025 f8193 f200000 fbig; do cp "$W/src/$n" "$W/mir/$n"; done

same() {			# compare one file in the image to its mirror
	if ! "$NEXTUFS" "$IMG" cat "/$1" | cmp -s - "$W/mir/$1"; then
		echo "FAIL  content differs for $1"
		fail=1
	fi
}

trunc() {			# truncate <name> <size>, mirrored on the host
	"$NEXTUFS" "$IMG" truncate "/$1" "$2" || { echo "FAIL  truncate $1 $2"; fail=1; }
	truncate -s "$2" "$W/mir/$1"
	same "$1"
}

putat() {			# putat <name> <hostfile> <offset>, mirrored
	"$NEXTUFS" "$IMG" putat "$2" "/$1" "$3" || { echo "FAIL  putat $1 $3"; fail=1; }
	dd if="$2" of="$W/mir/$1" bs=1 seek="$3" conv=notrunc status=none
	same "$1"
}

trunc fbig 5000				# drops a double indirect tree
trunc f200000 100000			# back into the first indirect block
trunc f8193 3000			# a full tail block becomes a short one
trunc f1025 1024
trunc f1025 0
check "after shrinking files"

trunc f1 9000				# a short tail grows past its block
trunc f1 200000				# and into the indirect blocks
check "after growing files"

: > "$W/mir/sp1"; : > "$W/mir/sp2"; : > "$W/mir/sp3"
putat sp1 "$W/src/f1024" 100000		# a file that is all hole up to 100000
putat sp1 "$W/src/f1023" 50100		# a partial chunk inside a hole
head -c 100 /dev/urandom > "$W/mir/sp2"
"$NEXTUFS" "$IMG" put "$W/mir/sp2" /sp2
putat sp2 "$W/src/f1024" 5000		# past the short tail, same block
head -c 100 /dev/urandom > "$W/mir/sp3"
"$NEXTUFS" "$IMG" put "$W/mir/sp3" /sp3
putat sp3 "$W/src/f1024" 20000		# past the short tail, a later block
check "after sparse writes"

# --- directories --------------------------------------------------------
"$NEXTUFS" "$IMG" mkdir /a
"$NEXTUFS" "$IMG" mkdir /a/b
"$NEXTUFS" "$IMG" mkdir /a/b/c
"$NEXTUFS" "$IMG" put "$W/src/f1024" /a/b/c/deep
check "after nested mkdir"

# many entries, to force the directory past one 1024-byte block
for i in $(seq 1 200); do
	"$NEXTUFS" "$IMG" put "$W/src/f1" "/a/entry-with-a-longish-name-$i"
done
check "after 200 entries in one directory"

"$NEXTUFS" "$IMG" ln -s /a/b/c/deep /a/link
"$NEXTUFS" "$IMG" ln -s "$(printf 'x%.0s' $(seq 1 200))" /a/longlink
check "after symlinks (fast and slow)"

[ "$("$NEXTUFS" "$IMG" readlink /a/link)" = /a/b/c/deep ] || { echo "FAIL readlink"; fail=1; }

# --- rename, permissions, whole trees -----------------------------------
mkdir -p "$W/tree/a/b"
head -c 200000 /dev/urandom > "$W/tree/a/big"
echo hello > "$W/tree/a/b/small"
ln -s ../big "$W/tree/a/b/lnk"
"$NEXTUFS" "$IMG" put -r "$W/tree" /tree
check "after a recursive put"

"$NEXTUFS" "$IMG" get -r /tree "$W/back"
if diff -r "$W/tree" "$W/back" > /dev/null; then
	echo "ok    recursive get matches the source tree"
else
	echo "FAIL  recursive get differs"
	fail=1
fi

"$NEXTUFS" "$IMG" mv /tree/a/big /tree/moved
"$NEXTUFS" "$IMG" mv /tree/a /tree-renamed
"$NEXTUFS" "$IMG" chmod 600 /tree/moved
"$NEXTUFS" "$IMG" chown 20:20 /tree/moved
check "after mv, chmod and chown"
[ "$("$NEXTUFS" "$IMG" stat /tree/moved | awk '/^owner/{print $2}')" = 20:20 ] || \
	{ echo "FAIL chown"; fail=1; }

# --- rename replacing an existing target, and hard links -----------------
"$NEXTUFS" "$IMG" mkdir /rn
"$NEXTUFS" "$IMG" put "$W/src/f1024" /rn/one
"$NEXTUFS" "$IMG" put "$W/src/f1025" /rn/two
"$NEXTUFS" "$IMG" mv /rn/one /rn/two			# file over file
"$NEXTUFS" "$IMG" cat /rn/two | cmp -s - "$W/src/f1024" || \
	{ echo "FAIL  mv over a file kept the wrong contents"; fail=1; }
"$NEXTUFS" "$IMG" mkdir /rn/d1
"$NEXTUFS" "$IMG" mkdir /rn/d2
"$NEXTUFS" "$IMG" mv /rn/d1 /rn/d2			# dir over empty dir
"$NEXTUFS" "$IMG" mkdir /rn/d3
"$NEXTUFS" "$IMG" put "$W/src/f1" /rn/d3/keep
"$NEXTUFS" "$IMG" mv /rn/d2 /rn/d3 2>/dev/null && \
	{ echo "FAIL  mv over a non-empty directory succeeded"; fail=1; }
"$NEXTUFS" "$IMG" mv /rn/d2 /rn/d2/inside 2>/dev/null && \
	{ echo "FAIL  mv of a directory into itself succeeded"; fail=1; }
"$NEXTUFS" "$IMG" mv /rn/two /rn/d3 2>/dev/null && \
	{ echo "FAIL  mv of a file over a directory succeeded"; fail=1; }
"$NEXTUFS" "$IMG" mv /rn/d3 /rn/two 2>/dev/null && \
	{ echo "FAIL  mv of a directory over a file succeeded"; fail=1; }
check "after renames that replace the target"

"$NEXTUFS" "$IMG" ln /rn/two /rn/link
[ "$("$NEXTUFS" "$IMG" stat /rn/link | awk '/^links/{print $2}')" = 2 ] || \
	{ echo "FAIL  hard link count is not 2"; fail=1; }
"$NEXTUFS" "$IMG" rm /rn/two
"$NEXTUFS" "$IMG" cat /rn/link | cmp -s - "$W/src/f1024" || \
	{ echo "FAIL  hard link lost its contents"; fail=1; }
check "after a hard link"

"$NEXTUFS" "$IMG" rm /rn/link
"$NEXTUFS" "$IMG" rm /rn/d3/keep
"$NEXTUFS" "$IMG" rmdir /rn/d3
"$NEXTUFS" "$IMG" rmdir /rn/d2
"$NEXTUFS" "$IMG" rmdir /rn
check "after removing the rename tree"

"$NEXTUFS" "$IMG" rm /tree/moved
"$NEXTUFS" "$IMG" rm /tree-renamed/b/lnk
"$NEXTUFS" "$IMG" rm /tree-renamed/b/small
"$NEXTUFS" "$IMG" rmdir /tree-renamed/b
"$NEXTUFS" "$IMG" rmdir /tree-renamed
"$NEXTUFS" "$IMG" rmdir /tree
check "after removing the trees"

# --- removal ------------------------------------------------------------
for i in $(seq 1 200); do
	"$NEXTUFS" "$IMG" rm "/a/entry-with-a-longish-name-$i"
done
check "after removing 200 entries"

for f in "$W"/src/*; do
	"$NEXTUFS" "$IMG" rm "/$(basename "$f")"
done
for n in sp1 sp2 sp3; do
	"$NEXTUFS" "$IMG" rm "/$n"
done
check "after removing every file"

"$NEXTUFS" "$IMG" rm /a/link
"$NEXTUFS" "$IMG" rm /a/longlink
"$NEXTUFS" "$IMG" rm /a/b/c/deep
"$NEXTUFS" "$IMG" rmdir /a/b/c
"$NEXTUFS" "$IMG" rmdir /a/b
"$NEXTUFS" "$IMG" rmdir /a
check "after removing every directory"

# --- back to where we started ------------------------------------------
echo "--- free space returned to the starting value"
before=$("$NEXTUFS" "$SRC" info | grep '^free')
after=$("$NEXTUFS" "$IMG" info | grep '^free')
if [ "$before" = "$after" ]; then
	echo "ok    $after"
else
	echo "FAIL  started: $before"
	echo "      ended:   $after"
	fail=1
fi

# --- the original files are all still intact ----------------------------
echo "--- original contents untouched"
"$NEXTUFS" "$SRC" find > "$W/paths"
n=0
while IFS= read -r p; do
	m=$("$NEXTUFS" "$SRC" stat "$p" 2>/dev/null | awk '/^mode/{print $2}')
	case "$m" in -*) ;; *) continue;; esac
	a=$("$NEXTUFS" "$SRC" cat "$p" | md5sum)
	b=$("$NEXTUFS" "$IMG" cat "$p" | md5sum)
	[ "$a" = "$b" ] || { echo "FAIL  $p changed"; fail=1; }
	n=$((n + 1))
done < "$W/paths"
echo "ok    $n original files unchanged"

[ $fail -eq 0 ] && echo "ALL PASS" || echo "FAILURES"
exit $fail
