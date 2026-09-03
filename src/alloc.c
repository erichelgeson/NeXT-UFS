/*
 * Block, fragment and inode allocation for the old 4.3BSD cylinder group.
 *
 * Everything here keeps five things in step, which is what makes NeXT's own
 * fsck accept the volume afterwards:
 *
 *	cg_free		the fragment bitmap (a set bit means free)
 *	cg_frsum[]	how many free runs of each fragment length exist
 *	cg_btot[]	free whole blocks per cylinder
 *	cg_b[][]	free whole blocks per cylinder and rotational position
 *	cg_cs / fs_cs / fs_cstotal	the summary counters
 */
#include "nextufs.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* --- bitmap primitives (a set bit means the fragment is free) ------------ */

static int
frag_isfree(const struct nufs *v, int bno)
{
	const uint8_t *map = v->cgbuf + CG_FREE;

	return (map[bno >> 3] >> (bno & 7)) & 1;
}

static void
frag_setfree(struct nufs *v, int bno)
{
	v->cgbuf[CG_FREE + (bno >> 3)] |= (uint8_t)(1 << (bno & 7));
}

static void
frag_clrfree(struct nufs *v, int bno)
{
	v->cgbuf[CG_FREE + (bno >> 3)] &= (uint8_t)~(1 << (bno & 7));
}

/* Is the whole block starting at cg-relative fragment bno free? */
static int
block_isfree(const struct nufs *v, int bno)
{
	int i;

	for (i = 0; i < v->frag; i++)
		if (!frag_isfree(v, bno + i))
			return 0;
	return 1;
}

static int
cyl_of(const struct nufs *v, int bno)
{
	return bno * v->nspf / v->spc;
}

static int
rpos_of(const struct nufs *v, int bno)
{
	return bno * v->nspf % v->spc % v->nsect * NUFS_NRPOS / v->nsect;
}

/* --- summary counters --------------------------------------------------- */

enum { CS_NDIR = 0, CS_NBFREE = 4, CS_NIFREE = 8, CS_NFFREE = 12 };

static void
bump(struct nufs *v, void *base, int off, int delta)
{
	nufs_put32(v, base, off, (uint32_t)((int32_t)nufs_get32(v, base, off) +
	    delta));
}

/* Adjust one summary field in the cg, the fs_cs array and fs_cstotal. */
static void
cs_adjust(struct nufs *v, int cg, int field, int delta)
{
	bump(v, v->cgbuf, CG_CS + field, delta);
	bump(v, v->csum, cg * 16 + field, delta);
	bump(v, v->sb, FS_CSTOTAL + field, delta);
	v->dirty_cg = v->dirty_csum = v->dirty_sb = 1;
}

static void
btot_adjust(struct nufs *v, int bno, int delta)
{
	int cyl = cyl_of(v, bno), rpos = rpos_of(v, bno);
	int o;

	if (cyl < 0 || cyl >= NUFS_MAXCPG)
		return;
	bump(v, v->cgbuf, CG_BTOT + 4 * cyl, delta);
	o = CG_B + 2 * (cyl * NUFS_NRPOS + rpos);
	nufs_put16(v, v->cgbuf, o,
	    (uint16_t)((int16_t)nufs_get16(v, v->cgbuf, o) + delta));
	v->dirty_cg = 1;
}

static void
frsum_adjust(struct nufs *v, int size, int delta)
{
	if (size <= 0 || size >= v->frag)
		return;
	bump(v, v->cgbuf, CG_FRSUM + 4 * size, delta);
	v->dirty_cg = 1;
}

/*
 * Add (sign +1) or remove (sign -1) the free-run histogram of the block
 * starting at cg-relative fragment `base`. Call it before and after touching
 * the bitmap and cg_frsum stays exact, however the runs are arranged.
 * A block that is entirely free contributes nothing: it is counted in
 * cg_btot / nbfree instead.
 */
static void
block_frsum(struct nufs *v, int base, int sign)
{
	int i, run = 0;

	for (i = base; i < base + v->frag; i++) {
		if (frag_isfree(v, i))
			run++;
		else {
			frsum_adjust(v, run, sign);
			run = 0;
		}
	}
	frsum_adjust(v, run, sign);
}

void
nufs_touch(struct nufs *v)
{
	nufs_put32(v, v->sb, FS_TIME, (uint32_t)time(NULL));
	v->sb[FS_FMOD] = 1;
	v->dirty_sb = 1;
}

/* --- whole-block allocation --------------------------------------------- */

/* Take the whole block at cg-relative fragment bno out of the free map. */
static void
block_take(struct nufs *v, int cg, int bno)
{
	int i;

	for (i = 0; i < v->frag; i++)
		frag_clrfree(v, bno + i);
	btot_adjust(v, bno, -1);
	cs_adjust(v, cg, CS_NBFREE, -1);
}

/* Put a whole free block back into the free map. */
static void
block_give(struct nufs *v, int cg, int bno)
{
	int i;

	for (i = 0; i < v->frag; i++)
		frag_setfree(v, bno + i);
	btot_adjust(v, bno, 1);
	cs_adjust(v, cg, CS_NBFREE, 1);
}

static int
cg_ndblk(const struct nufs *v)
{
	return (int32_t)nufs_get32(v, v->cgbuf, CG_NDBLK);
}

/* Find a free whole block in the loaded cg; returns a cg-relative frag or -1. */
static int
find_free_block(struct nufs *v)
{
	int ndblk = cg_ndblk(v);
	int start = (int32_t)nufs_get32(v, v->cgbuf, CG_ROTOR);
	int pass, bno;

	start -= start % v->frag;
	for (pass = 0; pass < 2; pass++) {
		int from = pass == 0 ? start : 0;
		int to = pass == 0 ? ndblk : start;

		for (bno = from; bno + v->frag <= to; bno += v->frag)
			if (block_isfree(v, bno))
				return bno;
	}
	return -1;
}

int
nufs_cgbase(const struct nufs *v, int cg)
{
	return v->fpg * cg;
}

/*
 * Allocate one whole block, preferring cylinder group `pref`.
 * Returns an absolute fragment address, or 0 on failure.
 */
int
nufs_alloc_block(struct nufs *v, int pref)
{
	int i, cg, bno;

	if (pref < 0 || pref >= v->ncg)
		pref = 0;
	for (i = 0; i < v->ncg; i++) {
		cg = (pref + i) % v->ncg;
		if (nufs_cg_load(v, cg) != 0)
			return 0;
		if ((int32_t)nufs_get32(v, v->csum, cg * 16 + CS_NBFREE) <= 0)
			continue;
		bno = find_free_block(v);
		if (bno < 0)
			continue;
		block_take(v, cg, bno);
		nufs_put32(v, v->cgbuf, CG_ROTOR, (uint32_t)bno);
		v->dirty_cg = 1;
		nufs_touch(v);
		return nufs_cgbase(v, cg) + bno;
	}
	nufs_err(v, "no free blocks left");
	return 0;
}

/*
 * Allocate `n` contiguous fragments (1 <= n <= fs_frag), preferring cylinder
 * group `pref`. Returns an absolute fragment address, or 0 on failure.
 */
int
nufs_alloc_frags(struct nufs *v, int pref, int n)
{
	int i, cg, ndblk, bno, run, base;

	if (n <= 0 || n > v->frag) {
		nufs_err(v, "bad fragment count %d", n);
		return 0;
	}
	if (n == v->frag)
		return nufs_alloc_block(v, pref);
	if (pref < 0 || pref >= v->ncg)
		pref = 0;

	for (i = 0; i < v->ncg; i++) {
		cg = (pref + i) % v->ncg;
		if (nufs_cg_load(v, cg) != 0)
			return 0;
		ndblk = cg_ndblk(v);

		/* First choice: carve the run out of an already broken block. */
		for (base = 0; base + v->frag <= ndblk; base += v->frag) {
			int first = -1;

			if (block_isfree(v, base))
				continue;
			run = 0;
			for (bno = base; bno < base + v->frag; bno++) {
				if (!frag_isfree(v, bno)) {
					run = 0;
					continue;
				}
				if (++run == n) {
					first = bno - n + 1;
					break;
				}
			}
			if (first < 0)
				continue;
			block_frsum(v, base, -1);
			for (run = 0; run < n; run++)
				frag_clrfree(v, first + run);
			block_frsum(v, base, 1);
			cs_adjust(v, cg, CS_NFFREE, -n);
			nufs_put32(v, v->cgbuf, CG_FROTOR, (uint32_t)first);
			v->dirty_cg = 1;
			nufs_touch(v);
			return nufs_cgbase(v, cg) + first;
		}

		/* Second choice: break up a whole free block. */
		if ((int32_t)nufs_get32(v, v->csum, cg * 16 + CS_NBFREE) <= 0)
			continue;
		bno = find_free_block(v);
		if (bno < 0)
			continue;
		block_take(v, cg, bno);
		for (run = n; run < v->frag; run++)
			frag_setfree(v, bno + run);
		block_frsum(v, bno, 1);
		cs_adjust(v, cg, CS_NFFREE, v->frag - n);
		nufs_put32(v, v->cgbuf, CG_FROTOR, (uint32_t)bno);
		v->dirty_cg = 1;
		nufs_touch(v);
		return nufs_cgbase(v, cg) + bno;
	}
	nufs_err(v, "no free fragments left");
	return 0;
}

/*
 * Free `n` fragments starting at absolute fragment address `frag`.
 * Coalescing a block back to whole is handled here.
 */
int
nufs_free_frags(struct nufs *v, int frag, int n)
{
	int cg = frag / v->fpg;
	int bno = frag % v->fpg;
	int base, i, nowfree;

	if (n <= 0)
		return 0;
	if (cg < 0 || cg >= v->ncg) {
		nufs_err(v, "cannot free fragment %d: out of range", frag);
		return -1;
	}
	if (nufs_cg_load(v, cg) != 0)
		return -1;
	if (bno < 0 || bno + n > cg_ndblk(v)) {
		nufs_err(v, "cannot free fragment %d: outside its group", frag);
		return -1;
	}
	for (i = 0; i < n; i++)
		if (frag_isfree(v, bno + i)) {
			nufs_err(v, "fragment %d freed twice", frag + i);
			return -1;
		}

	if (n == v->frag && (bno % v->frag) == 0) {
		block_give(v, cg, bno);
		nufs_touch(v);
		return 0;
	}

	base = bno - (bno % v->frag);
	block_frsum(v, base, -1);
	for (i = 0; i < n; i++)
		frag_setfree(v, bno + i);
	cs_adjust(v, cg, CS_NFFREE, n);

	nowfree = 0;
	for (i = base; i < base + v->frag; i++)
		if (frag_isfree(v, i))
			nowfree++;
	if (nowfree == v->frag) {
		/* The block is whole again: it moves from nffree to nbfree. */
		cs_adjust(v, cg, CS_NFFREE, -v->frag);
		btot_adjust(v, base, 1);
		cs_adjust(v, cg, CS_NBFREE, 1);
	} else
		block_frsum(v, base, 1);
	nufs_touch(v);
	return 0;
}

/* --- inode allocation --------------------------------------------------- */

static int
iused_get(const struct nufs *v, int i)
{
	return (v->cgbuf[CG_IUSED + (i >> 3)] >> (i & 7)) & 1;
}

static void
iused_set(struct nufs *v, int i, int on)
{
	if (on)
		v->cgbuf[CG_IUSED + (i >> 3)] |= (uint8_t)(1 << (i & 7));
	else
		v->cgbuf[CG_IUSED + (i >> 3)] &= (uint8_t)~(1 << (i & 7));
	v->dirty_cg = 1;
}

/* Allocate an inode, preferring cylinder group `pref`. 0 means failure. */
uint32_t
nufs_alloc_inode(struct nufs *v, int pref, int isdir)
{
	int i, cg, n;

	if (pref < 0 || pref >= v->ncg)
		pref = 0;
	for (i = 0; i < v->ncg; i++) {
		cg = (pref + i) % v->ncg;
		if (nufs_cg_load(v, cg) != 0)
			return 0;
		if ((int32_t)nufs_get32(v, v->csum, cg * 16 + CS_NIFREE) <= 0)
			continue;
		for (n = 0; n < v->ipg; n++) {
			if (iused_get(v, n))
				continue;
			if (cg == 0 && n < NUFS_ROOTINO)
				continue;
			iused_set(v, n, 1);
			nufs_put32(v, v->cgbuf, CG_IROTOR, (uint32_t)n);
			cs_adjust(v, cg, CS_NIFREE, -1);
			if (isdir)
				cs_adjust(v, cg, CS_NDIR, 1);
			nufs_touch(v);
			return (uint32_t)(cg * v->ipg + n);
		}
	}
	nufs_err(v, "no free inodes left");
	return 0;
}

int
nufs_free_inode(struct nufs *v, uint32_t ino, int isdir)
{
	int cg = (int)(ino / (uint32_t)v->ipg);
	int n = (int)(ino % (uint32_t)v->ipg);

	if (cg < 0 || cg >= v->ncg) {
		nufs_err(v, "inode %u out of range", ino);
		return -1;
	}
	if (nufs_cg_load(v, cg) != 0)
		return -1;
	if (!iused_get(v, n)) {
		nufs_err(v, "inode %u was already free", ino);
		return -1;
	}
	iused_set(v, n, 0);
	cs_adjust(v, cg, CS_NIFREE, 1);
	if (isdir)
		cs_adjust(v, cg, CS_NDIR, -1);
	nufs_touch(v);
	return 0;
}
