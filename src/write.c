/*
 * Growing, shrinking and writing files.
 *
 * di_blocks on NeXT counts fs_fsize fragments (fs_fsbtodb is 0 on every NeXT
 * volume, so a "disk block" and a fragment are the same thing); the shift is
 * applied anyway so an odd volume still comes out right.
 */
#include "nextufs.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOWMANY(x, y)	(((x) + (y) - 1) / (y))

static void
blocks_add(struct nufs *v, struct nufs_dinode *dp, int nfrags)
{
	dp->blocks = (uint32_t)((int32_t)dp->blocks + (nfrags << v->fsbtodb));
}

/*
 * How many fragments logical block `lbn` holds in a file of `size`.
 *
 * This is NeXT's blksize() macro (bsd/ufs/fs.h): only one of the twelve direct
 * blocks may be a short, fragmented tail. Anything at or past NDADDR is always
 * a whole block, however little of it the file uses. Getting this wrong hands
 * the leftover fragments of a big file's tail to the next allocation, and
 * NeXT's fsck reports them as DUP blocks.
 */
int
nufs_lbn_frags(const struct nufs *v, uint64_t size, int lbn)
{
	uint64_t start = (uint64_t)lbn * v->bsize;

	if (size <= start)
		return 0;
	if (lbn >= NUFS_NDADDR || size >= start + (uint64_t)v->bsize)
		return v->frag;
	return (int)HOWMANY(size - start, (uint64_t)v->fsize);
}

/* Fragments allocated for the last block of a file. */
int
nufs_tail_frags(const struct nufs *v, const struct nufs_dinode *dp)
{
	if (dp->size == 0 || (dp->flags & NUFS_IC_FASTLINK) != 0)
		return 0;
	return nufs_lbn_frags(v, dp->size, (int)((dp->size - 1) / v->bsize));
}

/* Fragments allocated for logical block `lbn` of this file. */
int
nufs_blk_frags(const struct nufs *v, const struct nufs_dinode *dp, int lbn)
{
	if ((dp->flags & NUFS_IC_FASTLINK) != 0)
		return 0;
	return nufs_lbn_frags(v, dp->size, lbn);
}

static int
prefcg(const struct nufs *v, const struct nufs_dinode *dp)
{
	return (int)(dp->ino / (uint32_t)v->ipg);
}

/* Allocate a zero-filled indirect block. */
static int
alloc_indirect(struct nufs *v, struct nufs_dinode *dp)
{
	uint8_t *zero;
	int blk;

	blk = nufs_alloc_block(v, prefcg(v, dp));
	if (blk == 0)
		return 0;
	zero = calloc(1, (size_t)v->bsize);
	if (zero == NULL) {
		nufs_errc(v, ENOMEM, "out of memory");
		return 0;
	}
	if (nufs_write_frags(v, blk, v->frag, zero) != 0) {
		free(zero);
		return 0;
	}
	free(zero);
	blocks_add(v, dp, v->frag);
	return blk;
}

/*
 * Resolve logical block `lbn` to a fragment address, allocating the block
 * (and any indirect blocks on the way) when `alloc` is set. `nfrags` says how
 * large a new block should be. Returns 0 and sets *out to 0 for a hole.
 */
static int
map_block(struct nufs *v, struct nufs_dinode *dp, int lbn, int alloc,
    int nfrags, int *out)
{
	int idx[NUFS_NIADDR], nlevels, level, i, blk, parent;
	uint8_t *buf;
	long long span;

	*out = 0;
	if (lbn < NUFS_NDADDR) {
		if (dp->db[lbn] == 0 && alloc) {
			blk = nufs_alloc_frags(v, prefcg(v, dp), nfrags);
			if (blk == 0)
				return -1;
			dp->db[lbn] = blk;
			blocks_add(v, dp, nfrags);
		}
		*out = dp->db[lbn];
		return 0;
	}

	lbn -= NUFS_NDADDR;
	for (nlevels = 1; nlevels <= NUFS_NIADDR; nlevels++) {
		span = 1;
		for (i = 0; i < nlevels; i++)
			span *= v->nindir;
		if (lbn < span)
			break;
		lbn -= (int)span;
	}
	if (nlevels > NUFS_NIADDR) {
		nufs_errc(v, EFBIG,
		    "file is too large for a triple indirect block");
		return -1;
	}
	for (level = nlevels - 1, i = 0; level >= 0; level--, i++) {
		int j;

		span = 1;
		for (j = 0; j < level; j++)
			span *= v->nindir;
		idx[i] = (int)(lbn / span);
		lbn = (int)(lbn % span);
	}

	if (dp->ib[nlevels - 1] == 0) {
		if (!alloc)
			return 0;
		blk = alloc_indirect(v, dp);
		if (blk == 0)
			return -1;
		dp->ib[nlevels - 1] = blk;
	}
	blk = dp->ib[nlevels - 1];

	buf = malloc((size_t)v->bsize);
	if (buf == NULL) {
		nufs_errc(v, ENOMEM, "out of memory");
		return -1;
	}
	for (i = 0; i < nlevels; i++) {
		int child;

		parent = blk;
		if (nufs_read_frags(v, parent, v->frag, buf) != 0)
			goto fail;
		child = (int32_t)nufs_get32(v, buf, 4 * idx[i]);
		if (child == 0) {
			if (!alloc) {
				free(buf);
				return 0;
			}
			child = (i == nlevels - 1) ?
			    nufs_alloc_frags(v, prefcg(v, dp), nfrags) :
			    alloc_indirect(v, dp);
			if (child == 0)
				goto fail;
			if (i == nlevels - 1)
				blocks_add(v, dp, nfrags);
			/* re-read: allocation may have moved the cg buffer */
			if (nufs_read_frags(v, parent, v->frag, buf) != 0)
				goto fail;
			nufs_put32(v, buf, 4 * idx[i], (uint32_t)child);
			if (nufs_write_frags(v, parent, v->frag, buf) != 0)
				goto fail;
		}
		blk = child;
	}
	free(buf);
	*out = blk;
	return 0;

fail:
	free(buf);
	return -1;
}

int
nufs_bmap_alloc(struct nufs *v, struct nufs_dinode *dp, int lbn, int nfrags,
    int *out)
{
	return map_block(v, dp, lbn, 1, nfrags, out);
}

/*
 * Make sure logical block `lbn` holds `want` fragments, growing the tail
 * block by relocating it when it has to get bigger.
 */
static int
ensure_block(struct nufs *v, struct nufs_dinode *dp, int lbn, int want,
    int *out, int *fresh)
{
	int have = nufs_blk_frags(v, dp, lbn);
	int old = 0, new, rc;
	uint8_t *buf;

	if (fresh != NULL)
		*fresh = 0;
	if (map_block(v, dp, lbn, 0, 0, &old) != 0)
		return -1;
	if (old == 0) {
		/*
		 * A hole. map_block hands back a block full of whatever was
		 * there before, so the caller must not read it: everything it
		 * holds is now zero by definition.
		 */
		if (fresh != NULL)
			*fresh = 1;
		return map_block(v, dp, lbn, 1, want, out);
	}
	if (want <= have) {
		*out = old;
		return 0;
	}

	/* Relocate the tail block into a larger run of fragments. */
	new = nufs_alloc_frags(v, prefcg(v, dp), want);
	if (new == 0)
		return -1;
	buf = calloc(1, (size_t)v->bsize);
	if (buf == NULL) {
		nufs_errc(v, ENOMEM, "out of memory");
		return -1;
	}
	rc = nufs_read_frags(v, old, have, buf);
	if (rc == 0)
		rc = nufs_write_frags(v, new, want, buf);
	free(buf);
	if (rc != 0)
		return -1;
	if (nufs_free_frags(v, old, have) != 0)
		return -1;
	blocks_add(v, dp, want - have);

	/* Re-point the parent at the new address. */
	if (lbn < NUFS_NDADDR)
		dp->db[lbn] = new;
	else {
		/*
		 * Only the last block of a file is ever grown, and a file that
		 * far in has full blocks behind it, so this only happens for
		 * direct blocks in practice. Handle it anyway by rewriting the
		 * indirect entry through a fresh mapping walk.
		 */
		int cur;

		if (map_block(v, dp, lbn, 0, 0, &cur) != 0)
			return -1;
		nufs_err(v, "cannot relocate an indirect tail block");
		return -1;
	}
	*out = new;
	return 0;
}

/*
 * A file's last block is the only one allowed to be short. Before anything
 * grows past it, widen it to the size a block at that position needs in a
 * file of `endsize` bytes, so its leftover fragments are never handed out
 * twice. Holes stay holes.
 */
static int
grow_tail(struct nufs *v, struct nufs_dinode *dp, uint64_t endsize)
{
	uint64_t end;
	int oldlast, want, have, frag;

	if (dp->size == 0 || (dp->flags & NUFS_IC_FASTLINK) != 0)
		return 0;
	oldlast = (int)((dp->size - 1) / v->bsize);
	want = nufs_lbn_frags(v, endsize, oldlast);
	have = nufs_blk_frags(v, dp, oldlast);
	if (want <= have)
		return 0;
	if (map_block(v, dp, oldlast, 0, 0, &frag) != 0)
		return -1;
	if (frag == 0)
		return 0;
	if (ensure_block(v, dp, oldlast, want, &frag, NULL) != 0)
		return -1;
	/*
	 * How many fragments a block holds is derived from di_size, so the
	 * size has to follow the widened tail or the next lookup would think
	 * the block is still short and relocate it a second time. The new
	 * bytes are zero: ensure_block pads the block it copies.
	 */
	end = (uint64_t)(oldlast + 1) * v->bsize;
	dp->size = endsize < end ? endsize : end;
	return 0;
}

long
nufs_file_write(struct nufs *v, struct nufs_dinode *dp, const void *vbuf,
    long long off, long n)
{
	const uint8_t *in = vbuf;
	uint8_t *blk;
	long done = 0;
	uint64_t endsize;

	if (n <= 0)
		return 0;
	endsize = (uint64_t)off + (uint64_t)n;
	if (endsize < dp->size)
		endsize = dp->size;

	if (off > (long long)dp->size && grow_tail(v, dp, endsize) != 0)
		return -1;

	blk = malloc((size_t)v->bsize);
	if (blk == NULL) {
		nufs_errc(v, ENOMEM, "out of memory");
		return -1;
	}
	while (done < n) {
		long long pos = off + done;
		int lbn = (int)(pos / v->bsize);
		int boff = (int)(pos % v->bsize);
		long chunk = v->bsize - boff;
		int want, frag, have, fresh;

		if (chunk > n - done)
			chunk = n - done;
		want = nufs_lbn_frags(v, endsize, lbn);
		if (ensure_block(v, dp, lbn, want, &frag, &fresh) != 0)
			goto fail;
		have = nufs_blk_frags(v, dp, lbn);
		if (have > want)
			have = want;

		memset(blk, 0, (size_t)v->bsize);
		if (!fresh && have > 0 &&
		    (boff != 0 || chunk < (long)have * v->fsize)) {
			if (nufs_read_frags(v, frag, have, blk) != 0)
				goto fail;
		}
		memcpy(blk + boff, in + done, (size_t)chunk);
		if (nufs_write_frags(v, frag, want, blk) != 0)
			goto fail;

		done += chunk;
		if ((uint64_t)(off + done) > dp->size)
			dp->size = (uint64_t)(off + done);
	}
	free(blk);
	dp->mtime = dp->ctime = (uint32_t)time(NULL);
	if (nufs_inode_write(v, dp) != 0)
		return -1;
	return done;

fail:
	/*
	 * A write that stops partway -- running out of space is the way this
	 * happens -- has usually allocated the block it died on. Save the
	 * bytes that did land, then truncate to that size, which hands the
	 * unfilled block back. Leaving it would strand blocks that no inode
	 * claims and fsck would have to find.
	 */
	free(blk);
	dp->mtime = dp->ctime = (uint32_t)time(NULL);
	if (nufs_inode_write(v, dp) == 0) {
		char saved[sizeof(v->err)];
		int savederr = v->errnum;

		snprintf(saved, sizeof(saved), "%s", v->err);
		nufs_truncate(v, dp, dp->size);
		snprintf(v->err, sizeof(v->err), "%s", saved);
		v->errnum = savederr;
	}
	return -1;
}

/* --- truncation --------------------------------------------------------- */

/* How many logical blocks one entry of a `level`-deep indirect block covers. */
static long long
indir_span(const struct nufs *v, int level)
{
	long long span = 1;
	int i;

	for (i = 0; i < level; i++)
		span *= v->nindir;
	return span;
}

/* Free an indirect block, everything below it, and the data it points at. */
static int
free_tree(struct nufs *v, struct nufs_dinode *dp, int blk, int level)
{
	uint8_t *buf;
	int i;

	if (blk == 0)
		return 0;
	buf = malloc((size_t)v->bsize);
	if (buf == NULL) {
		nufs_errc(v, ENOMEM, "out of memory");
		return -1;
	}
	if (nufs_read_frags(v, blk, v->frag, buf) != 0) {
		free(buf);
		return -1;
	}
	for (i = 0; i < v->nindir; i++) {
		int child = (int32_t)nufs_get32(v, buf, 4 * i);
		int rc;

		if (child == 0)
			continue;
		if (level > 0)
			rc = free_tree(v, dp, child, level - 1);
		else if ((rc = nufs_free_frags(v, child, v->frag)) == 0)
			blocks_add(v, dp, -v->frag);
		if (rc != 0) {
			free(buf);
			return -1;
		}
	}
	free(buf);
	if (nufs_free_frags(v, blk, v->frag) != 0)
		return -1;
	blocks_add(v, dp, -v->frag);
	return 0;
}

/*
 * Keep the first `keep` logical blocks of the tree hanging off *slot and
 * release the rest, clearing every pointer that goes away. The tree itself
 * goes when nothing in it is kept.
 */
static int
prune_tree(struct nufs *v, struct nufs_dinode *dp, int32_t *slot, int level,
    long long keep)
{
	long long per = indir_span(v, level);
	uint8_t *buf;
	int i, modified = 0;

	if (*slot == 0)
		return 0;
	if (keep <= 0) {
		if (free_tree(v, dp, *slot, level) != 0)
			return -1;
		*slot = 0;
		return 0;
	}
	buf = malloc((size_t)v->bsize);
	if (buf == NULL) {
		nufs_errc(v, ENOMEM, "out of memory");
		return -1;
	}
	if (nufs_read_frags(v, *slot, v->frag, buf) != 0)
		goto fail;
	for (i = 0; i < v->nindir; i++) {
		long long ckeep = keep - (long long)i * per;
		int32_t child = (int32_t)nufs_get32(v, buf, 4 * i);

		if (ckeep >= per)			/* wholly kept */
			continue;
		if (child == 0)
			continue;
		if (level == 0) {
			if (nufs_free_frags(v, child, v->frag) != 0)
				goto fail;
			blocks_add(v, dp, -v->frag);
			child = 0;
		} else {
			int32_t was = child;

			if (prune_tree(v, dp, &child, level - 1, ckeep) != 0)
				goto fail;
			if (child == was)
				continue;
		}
		nufs_put32(v, buf, 4 * i, (uint32_t)child);
		modified = 1;
	}
	if (modified && nufs_write_frags(v, *slot, v->frag, buf) != 0)
		goto fail;
	free(buf);
	return 0;

fail:
	free(buf);
	return -1;
}

/*
 * Zero the part of the last block that lies past the new end of file.
 * nufs_file_write trusts that everything inside an allocated block past
 * di_size is already zero; without this a later write into the shortened
 * tail would expose whatever the file used to hold there.
 */
static int
zero_tail(struct nufs *v, struct nufs_dinode *dp, uint64_t newsize)
{
	int lbn, boff, nfrags, frag, end;
	uint8_t *buf;

	if (newsize == 0)
		return 0;
	boff = (int)(newsize % (uint64_t)v->bsize);
	if (boff == 0)
		return 0;			/* the block ends with the file */
	lbn = (int)((newsize - 1) / v->bsize);
	nfrags = nufs_lbn_frags(v, newsize, lbn);
	end = nfrags * v->fsize;
	if (end <= boff)
		return 0;
	if (map_block(v, dp, lbn, 0, 0, &frag) != 0)
		return -1;
	if (frag == 0)
		return 0;			/* a hole reads as zero already */
	buf = malloc((size_t)v->bsize);
	if (buf == NULL) {
		nufs_errc(v, ENOMEM, "out of memory");
		return -1;
	}
	if (nufs_read_frags(v, frag, nfrags, buf) != 0) {
		free(buf);
		return -1;
	}
	memset(buf + boff, 0, (size_t)(end - boff));
	if (nufs_write_frags(v, frag, nfrags, buf) != 0) {
		free(buf);
		return -1;
	}
	free(buf);
	return 0;
}

/* Release everything past `newsize`, keeping what is still inside the file. */
static int
shrink(struct nufs *v, struct nufs_dinode *dp, uint64_t newsize)
{
	uint64_t oldsize = dp->size;
	long long start;
	int oldlast, newlast, lbn, i, old, new, frag;

	oldlast = (int)((oldsize - 1) / v->bsize);
	newlast = newsize == 0 ? -1 : (int)((newsize - 1) / v->bsize);

	for (lbn = newlast + 1; lbn < NUFS_NDADDR && lbn <= oldlast; lbn++) {
		if (dp->db[lbn] == 0)
			continue;
		old = nufs_lbn_frags(v, oldsize, lbn);
		if (nufs_free_frags(v, dp->db[lbn], old) != 0)
			return -1;
		blocks_add(v, dp, -old);
		dp->db[lbn] = 0;
	}

	start = NUFS_NDADDR;
	for (i = 0; i < NUFS_NIADDR; i++) {
		long long span = indir_span(v, i + 1);

		if (prune_tree(v, dp, &dp->ib[i], i,
		    (long long)(newlast + 1) - start) != 0)
			return -1;
		start += span;
	}

	/* The surviving tail may itself need fewer fragments than it holds. */
	if (newlast >= 0 && newlast < NUFS_NDADDR) {
		old = nufs_lbn_frags(v, oldsize, newlast);
		new = nufs_lbn_frags(v, newsize, newlast);
		if (new < old && dp->db[newlast] != 0) {
			frag = dp->db[newlast];
			if (nufs_free_frags(v, frag + new, old - new) != 0)
				return -1;
			blocks_add(v, dp, -(old - new));
		}
	}
	return zero_tail(v, dp, newsize);
}

int
nufs_truncate(struct nufs *v, struct nufs_dinode *dp, uint64_t newsize)
{
	if ((dp->flags & NUFS_IC_FASTLINK) != 0) {
		if (newsize != 0) {
			nufs_errc(v, EINVAL, "cannot resize an inline symlink");
			return -1;
		}
		dp->size = 0;
		dp->mtime = dp->ctime = (uint32_t)time(NULL);
		return nufs_inode_write(v, dp);
	}
	if (newsize == dp->size)
		return 0;

	if (newsize < dp->size) {
		if (shrink(v, dp, newsize) != 0)
			return -1;
		if (newsize == 0)
			dp->blocks = 0;
	} else if (grow_tail(v, dp, newsize) != 0)
		return -1;
	/* Growing leaves the new tail a hole; it reads back as zeros. */

	dp->size = newsize;
	dp->mtime = dp->ctime = (uint32_t)time(NULL);
	return nufs_inode_write(v, dp);
}
