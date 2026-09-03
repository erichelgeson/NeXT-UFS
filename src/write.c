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
    int *out)
{
	int have = nufs_blk_frags(v, dp, lbn);
	int old = 0, new, rc;
	uint8_t *buf;

	if (map_block(v, dp, lbn, 0, 0, &old) != 0)
		return -1;
	if (old == 0)
		return map_block(v, dp, lbn, 1, want, out);
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
		int want, frag, have;

		if (chunk > n - done)
			chunk = n - done;
		want = nufs_lbn_frags(v, endsize, lbn);
		if (ensure_block(v, dp, lbn, want, &frag) != 0)
			goto fail;
		have = nufs_blk_frags(v, dp, lbn);
		if (have > want)
			have = want;

		memset(blk, 0, (size_t)v->bsize);
		if (have > 0 && (boff != 0 || chunk < (long)have * v->fsize)) {
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
	free(blk);
	return -1;
}

/* --- truncation --------------------------------------------------------- */

/* Free the indirect blocks themselves; data blocks are released separately. */
static int
free_indirect(struct nufs *v, struct nufs_dinode *dp, int blk, int level)
{
	uint8_t *buf;
	int i;

	if (blk == 0)
		return 0;
	if (level > 0) {
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

			if (child != 0 &&
			    free_indirect(v, dp, child, level - 1) != 0) {
				free(buf);
				return -1;
			}
		}
		free(buf);
	}
	if (nufs_free_frags(v, blk, v->frag) != 0)
		return -1;
	blocks_add(v, dp, -v->frag);
	return 0;
}

/* Release everything a file holds and set its size to zero. */
int
nufs_truncate(struct nufs *v, struct nufs_dinode *dp, uint64_t newsize)
{
	int lbn, i, lastlbn, tail, frag;

	if (newsize != 0) {
		nufs_err(v, "only truncation to zero is implemented");
		return -1;
	}
	if ((dp->flags & NUFS_IC_FASTLINK) != 0) {
		dp->size = 0;
		return nufs_inode_write(v, dp);
	}

	lastlbn = (int)((dp->size + v->bsize - 1) / v->bsize);
	for (lbn = 0; lbn < lastlbn; lbn++) {
		if (map_block(v, dp, lbn, 0, 0, &frag) != 0)
			return -1;
		if (frag == 0)
			continue;
		tail = nufs_blk_frags(v, dp, lbn);
		if (nufs_free_frags(v, frag, tail) != 0)
			return -1;
		blocks_add(v, dp, -tail);
	}
	for (i = 0; i < NUFS_NIADDR; i++) {
		if (dp->ib[i] == 0)
			continue;
		if (free_indirect(v, dp, dp->ib[i], i) != 0)
			return -1;
		dp->ib[i] = 0;
	}
	for (i = 0; i < NUFS_NDADDR; i++)
		dp->db[i] = 0;
	dp->size = 0;
	dp->blocks = 0;
	dp->mtime = dp->ctime = (uint32_t)time(NULL);
	return nufs_inode_write(v, dp);
}
