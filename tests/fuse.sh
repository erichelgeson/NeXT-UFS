#!/usr/bin/env bash
# Exercise the FUSE driver against a mounted image, then check the volume the
# kernel left behind with nextufs fsck. Needs /dev/fuse, so it does not run in
# a sandbox: it exits 77 (skipped) when it cannot mount anything.
set -u

NEXTUFS=${NEXTUFS:-./build/nextufs}
FUSEBIN=${FUSEBIN:-./build/nextufs-fuse}
REAL=${1:-"HD1 - BlueSCSIToolboxNeXT.hda"}
W=$(mktemp -d "${TMPDIR:-/tmp}/nextufs-fuse.XXXXXX")
MNT=$W/mnt
fail=0

[ -x "$FUSEBIN" ] || { echo "skip: $FUSEBIN is not built (make fuse)"; exit 77; }
[ -c /dev/fuse ] || { echo "skip: no /dev/fuse"; exit 77; }
command -v fusermount3 >/dev/null || { echo "skip: no fusermount3"; exit 77; }

cleanup() {
	fusermount3 -u "$MNT" 2>/dev/null
	rm -rf "$W"
}
trap cleanup EXIT

ok()   { echo "ok    $1"; }
bad()  { echo "FAIL  $1"; fail=1; }
want() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got $2, want $3)"; fi; }

MPID=

mount_img() {			# mount_img <image> [options...]
	mkdir -p "$MNT"
	"$FUSEBIN" "$@" "$MNT" || return 1
	for _ in $(seq 1 50); do
		mountpoint -q "$MNT" && break
		sleep 0.1
	done
	mountpoint -q "$MNT" || return 1
	MPID=$(pgrep -f "nextufs-fuse.*$MNT" | head -1)
	return 0
}

# The daemon holds the image open until it has flushed and unlocked, so the
# CLI has to wait for the process itself, not just for the mount point.
umount_img() {
	fusermount3 -u "$MNT" || return 1
	for _ in $(seq 1 100); do
		if ! mountpoint -q "$MNT" &&
		    { [ -z "$MPID" ] || ! kill -0 "$MPID" 2>/dev/null; }; then
			MPID=
			return 0
		fi
		sleep 0.1
	done
	return 1
}

fsck_clean() {			# fsck_clean <what> <image>
	local out
	out=$("$NEXTUFS" "$2" fsck 2>&1)
	if grep -q '^0 problems found' <<<"$out"; then
		ok "$1"
	else
		bad "$1"
		sed 's/^/      /' <<<"$out"
	fi
}

# --- source files -------------------------------------------------------
mkdir -p "$W/src"
sizes="0 1 1023 1024 1025 4095 8191 8192 8193 98303 98304 106496 200000 1000000"
for s in $sizes; do head -c "$s" /dev/urandom > "$W/src/f$s"; done
head -c 17000000 /dev/urandom > "$W/src/fbig"

exercise() {			# exercise <image> — everything through the mount
	local img=$1 base avail0 avail1

	mount_img "$img" -o auto_unmount || { bad "mount $img"; return 1; }

	# 1. every size, in and back out
	mkdir "$MNT/sizes"
	for f in "$W"/src/*; do cp "$f" "$MNT/sizes/$(basename "$f")"; done
	for f in "$W"/src/*; do
		cmp -s "$f" "$MNT/sizes/$(basename "$f")" || \
		    bad "$(basename "$f") differs through the mount"
	done
	ok "copied and compared $(ls "$W/src" | wc -l) files"

	# 2. directories
	mkdir -p "$MNT/a/b/c"
	echo hello > "$MNT/a/b/c/deep"
	rm "$MNT/a/b/c/deep"
	rmdir "$MNT/a/b/c" "$MNT/a/b"
	for i in $(seq 1 200); do
		echo "$i" > "$MNT/a/entry-with-a-longish-name-$i"
	done
	want "200 entries in one directory" "$(ls "$MNT/a" | wc -l)" 200
	rm "$MNT"/a/entry-with-a-longish-name-*
	want "directory is empty again" "$(ls -A "$MNT/a" | wc -l)" 0

	# 3. rename
	cp "$W/src/f1024" "$MNT/one"
	cp "$W/src/f1025" "$MNT/two"
	mv "$MNT/one" "$MNT/two"
	cmp -s "$W/src/f1024" "$MNT/two" || bad "mv over a file"
	mkdir "$MNT/d1" "$MNT/d2" "$MNT/d3"
	mv -T "$MNT/d1" "$MNT/d2"			# over an empty directory
	echo x > "$MNT/d3/keep"
	mv -T "$MNT/d2" "$MNT/d2/inside" 2>/dev/null && bad "mv into own subtree"
	mv -T "$MNT/d2" "$MNT/d3" 2>/dev/null && bad "mv over a non-empty directory"
	mv -T "$MNT/two" "$MNT/d3" 2>/dev/null && bad "mv of a file over a directory"
	mv -T "$MNT/d3" "$MNT/two" 2>/dev/null && bad "mv of a directory over a file"
	ok "renames follow POSIX"

	# 4. truncate
	cp "$W/src/f1000000" "$MNT/t1"; cp "$W/src/f1000000" "$W/t1"
	cp "$W/src/fbig" "$MNT/t2";     cp "$W/src/fbig" "$W/t2"
	for sz in 5000 900000 1048576 0; do
		truncate -s "$sz" "$MNT/t1"; truncate -s "$sz" "$W/t1"
		cmp -s "$MNT/t1" "$W/t1" || bad "truncate t1 to $sz"
	done
	for sz in 5000000 20000000 3000 0; do
		truncate -s "$sz" "$MNT/t2"; truncate -s "$sz" "$W/t2"
		cmp -s "$MNT/t2" "$W/t2" || bad "truncate t2 to $sz"
	done
	ok "shrink, grow and zero"

	# 5. sparse writes
	: > "$MNT/sp"; : > "$W/sp"
	dd if="$W/src/f1024" of="$MNT/sp" bs=1024 seek=100 conv=notrunc status=none
	dd if="$W/src/f1024" of="$W/sp"   bs=1024 seek=100 conv=notrunc status=none
	cmp -s "$MNT/sp" "$W/sp" || bad "sparse write at 102400"
	head -c 1 /dev/urandom > "$MNT/sp2"; cp "$MNT/sp2" "$W/sp2"
	dd if="$W/src/f1024" of="$MNT/sp2" bs=1024 seek=20 conv=notrunc status=none
	dd if="$W/src/f1024" of="$W/sp2"   bs=1024 seek=20 conv=notrunc status=none
	cmp -s "$MNT/sp2" "$W/sp2" || bad "sparse write past a short tail"
	ok "sparse writes"

	# 6. links
	ln -s /a/b/c/short "$MNT/l1"
	ln -s "$(printf 'x%.0s' $(seq 1 200))" "$MNT/l2"
	want "short symlink" "$(readlink "$MNT/l1")" /a/b/c/short
	want "long symlink" "$(readlink "$MNT/l2" | wc -c)" 201
	cp "$W/src/f1024" "$MNT/h1"
	ln "$MNT/h1" "$MNT/h2"
	want "hard link count" "$(stat -c %h "$MNT/h1")" 2
	rm "$MNT/h1"
	cmp -s "$W/src/f1024" "$MNT/h2" || bad "hard link after removing the original"
	ok "hard link survives its first name"

	# 7. a fifo, the one mknod type a normal user can make
	mkfifo "$MNT/fifo"
	want "mkfifo" "$(stat -c %F "$MNT/fifo")" "fifo"
	rm "$MNT/fifo"

	# 8. metadata
	chmod 600 "$MNT/h2"
	chown "$(id -u):$(id -g)" "$MNT/h2" 2>/dev/null
	touch -d '2001-02-03 04:05:06 UTC' "$MNT/h2"
	want "chmod" "$(stat -c %a "$MNT/h2")" 600
	want "mtime" "$(stat -c %Y "$MNT/h2")" 981173106

	# 9. an open file that is unlinked
	cp "$W/src/f8193" "$MNT/gone"
	exec 3< "$MNT/gone"
	rm "$MNT/gone"
	cmp -s "$W/src/f8193" /dev/fd/3 || bad "reading an unlinked open file"
	exec 3<&-
	sleep 0.2
	want "no .fuse_hidden left behind" "$(ls -A "$MNT" | grep -c fuse_hidden)" 0

	# 10. free space moves
	avail0=$(df --output=avail "$MNT" | tail -1)
	cp "$W/src/f1000000" "$MNT/df1"
	avail1=$(df --output=avail "$MNT" | tail -1)
	if [ "$((avail0 - avail1))" -ge 900 ] && [ "$((avail0 - avail1))" -le 1200 ]; then
		ok "df moved by $((avail0 - avail1)) blocks for a 1 MB file"
	else
		bad "df moved by $((avail0 - avail1)) blocks for a 1 MB file"
	fi

	# 11. leave one file behind for the CLI to read, remove the rest
	cp "$W/src/f4095" "$MNT/witness"
	rm -rf "$MNT/sizes" "$MNT/a" "$MNT/d2" "$MNT/d3" "$MNT/t1" "$MNT/t2" \
	    "$MNT/sp" "$MNT/sp2" "$MNT/l1" "$MNT/l2" "$MNT/h2" "$MNT/two" \
	    "$MNT/df1"
	base=$(ls -A "$MNT" | grep -v '^witness$' | grep -vx 'lost+found' | \
	    grep -v '^\.NextTrash$' | grep -v '^BlueSCSI-CD$' | \
	    grep -v '^source$' | grep -v '^toolbox-cd$' | grep -v '^\.hidden$')
	[ -z "$base" ] || bad "left behind: $(tr '\n' ' ' <<<"$base")"

	umount_img || bad "unmount"
	fsck_clean "fsck after the mount" "$img"
	"$NEXTUFS" "$img" cat /witness | cmp -s - "$W/src/f4095" || \
	    bad "the CLI cannot read a file written through the mount"
	want "volume is marked clean" \
	    "$("$NEXTUFS" "$img" info | awk '/^state/{print $5}')" 1
	"$NEXTUFS" "$img" rm /witness
}

# --- a fresh volume ------------------------------------------------------
echo "--- fresh 64 MB volume"
"$NEXTUFS" "$W/fresh.hda" mkfs 64 FUSETEST > /dev/null
free0=$("$NEXTUFS" "$W/fresh.hda" info | grep '^free')
exercise "$W/fresh.hda"
free1=$("$NEXTUFS" "$W/fresh.hda" info | grep '^free')
want "free space returned to the baseline" "$free1" "$free0"

# --- a copy of the real volume ------------------------------------------
if [ -f "$REAL" ]; then
	echo "--- a copy of $REAL"
	cp "$REAL" "$W/real.hda"
	free0=$("$NEXTUFS" "$W/real.hda" info | grep '^free')
	exercise "$W/real.hda"
	free1=$("$NEXTUFS" "$W/real.hda" info | grep '^free')
	want "free space returned to the baseline" "$free1" "$free0"

	n=0
	"$NEXTUFS" "$REAL" find > "$W/paths"
	while IFS= read -r p; do
		m=$("$NEXTUFS" "$REAL" stat "$p" 2>/dev/null | awk '/^mode/{print $2}')
		case "$m" in -*) ;; *) continue;; esac
		a=$("$NEXTUFS" "$REAL" cat "$p" | md5sum)
		b=$("$NEXTUFS" "$W/real.hda" cat "$p" | md5sum)
		[ "$a" = "$b" ] || bad "$p changed"
		n=$((n + 1))
	done < "$W/paths"
	ok "$n original files unchanged"
else
	echo "--- $REAL is not here, skipping the real-volume pass"
fi

# --- read-only ----------------------------------------------------------
echo "--- read-only and recovery"
mount_img "$W/fresh.hda" -o ro || bad "read-only mount"
if touch "$MNT/nope" 2>/dev/null; then
	bad "a read-only mount accepted a write"
	rm -f "$MNT/nope"
else
	ok "a read-only mount refuses writes"
fi
want "read-only mount leaves the volume clean" \
    "$("$NEXTUFS" "$W/fresh.hda" info | awk '/^state/{print $5}')" 1
umount_img || bad "unmount the read-only mount"

# --- the CLI cannot touch a mounted image -------------------------------
mount_img "$W/fresh.hda" || bad "mount for the locking check"
if "$NEXTUFS" "$W/fresh.hda" mkdir /viacli 2>"$W/lockerr"; then
	bad "the CLI wrote to a mounted image"
else
	grep -q 'in use' "$W/lockerr" && ok "the CLI refuses a mounted image" || \
	    bad "wrong error for a mounted image: $(cat "$W/lockerr")"
fi

# --- kill -9 leaves the volume dirty ------------------------------------
pid=$(pgrep -f "nextufs-fuse.*$W/fresh.hda" | head -1)
if [ -n "$pid" ]; then
	kill -9 "$pid"
	sleep 0.3
	fusermount3 -u "$MNT" 2>/dev/null
	want "a killed mount leaves the volume dirty" \
	    "$("$NEXTUFS" "$W/fresh.hda" info | awk '/^state/{print $5}')" 2
	if mount_img "$W/fresh.hda" 2>"$W/dirtyerr"; then
		bad "a dirty volume mounted read-write without -o force"
		umount_img
	else
		ok "a dirty volume needs -o force"
	fi
	mount_img "$W/fresh.hda" -o force || bad "-o force did not mount"
	umount_img
	"$NEXTUFS" "$W/fresh.hda" fsck -y > /dev/null
	want "fsck -y marks it clean again" \
	    "$("$NEXTUFS" "$W/fresh.hda" info | awk '/^state/{print $5}')" 1
	fsck_clean "the recovered volume is consistent" "$W/fresh.hda"
else
	bad "could not find the mount daemon to kill"
	umount_img
fi

[ $fail -eq 0 ] && echo "ALL PASS" || echo "FAILURES"
exit $fail
