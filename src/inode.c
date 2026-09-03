#include "nextufs.h"
#include <string.h>
#include <stdlib.h>

static long long
inode_offset(const struct nufs *v, uint32_t ino)
{
	int cg = (int)(ino / (uint32_t)v->ipg);
	int frag = nufs_cgstart(v, cg) + v->iblkno +
	    (int)((ino % (uint32_t)v->ipg) / (uint32_t)v->inopb) * v->frag;

	return nufs_fragoff(v, frag) +
	    (long long)(ino % (uint32_t)v->inopb) * NUFS_DINODE_SIZE;
}

int
nufs_inode_read(struct nufs *v, uint32_t ino, struct nufs_dinode *dp)
{
	uint8_t raw[NUFS_DINODE_SIZE];
	int i;

	if (ino < NUFS_ROOTINO || ino >= (uint32_t)(v->ipg * v->ncg)) {
		nufs_err(v, "inode %u out of range", ino);
		return -1;
	}
	if (nufs_pread(v, raw, inode_offset(v, ino), sizeof(raw)) != 0)
		return -1;

	memset(dp, 0, sizeof(*dp));
	dp->ino = ino;
	dp->mode = nufs_get16(v, raw, DI_MODE);
	dp->nlink = (int16_t)nufs_get16(v, raw, DI_NLINK);
	dp->uid = nufs_get16(v, raw, DI_UID);
	dp->gid = nufs_get16(v, raw, DI_GID);
	if (v->be)
		dp->size = ((uint64_t)nufs_get32(v, raw, DI_SIZE) << 32) |
		    nufs_get32(v, raw, DI_SIZE + 4);
	else
		dp->size = ((uint64_t)nufs_get32(v, raw, DI_SIZE + 4) << 32) |
		    nufs_get32(v, raw, DI_SIZE);
	dp->atime = nufs_get32(v, raw, DI_ATIME);
	dp->mtime = nufs_get32(v, raw, DI_MTIME);
	dp->ctime = nufs_get32(v, raw, DI_CTIME);
	dp->flags = nufs_get32(v, raw, DI_FLAGS);
	dp->blocks = nufs_get32(v, raw, DI_BLOCKS);
	dp->gen = nufs_get32(v, raw, DI_GEN);
	for (i = 0; i < NUFS_NDADDR; i++)
		dp->db[i] = (int32_t)nufs_get32(v, raw, DI_DB + 4 * i);
	for (i = 0; i < NUFS_NIADDR; i++)
		dp->ib[i] = (int32_t)nufs_get32(v, raw, DI_IB + 4 * i);
	memcpy(dp->symlink, raw + DI_SYMLINK, NUFS_MAXFASTLINK);
	dp->symlink[NUFS_MAXFASTLINK] = '\0';
	return 0;
}

int
nufs_inode_write(struct nufs *v, const struct nufs_dinode *dp)
{
	uint8_t raw[NUFS_DINODE_SIZE];
	int i;

	/* Preserve the spare words and anything else we do not model. */
	if (nufs_pread(v, raw, inode_offset(v, dp->ino), sizeof(raw)) != 0)
		return -1;

	nufs_put16(v, raw, DI_MODE, dp->mode);
	nufs_put16(v, raw, DI_NLINK, (uint16_t)dp->nlink);
	nufs_put16(v, raw, DI_UID, dp->uid);
	nufs_put16(v, raw, DI_GID, dp->gid);
	if (v->be) {
		nufs_put32(v, raw, DI_SIZE, (uint32_t)(dp->size >> 32));
		nufs_put32(v, raw, DI_SIZE + 4, (uint32_t)dp->size);
	} else {
		nufs_put32(v, raw, DI_SIZE, (uint32_t)dp->size);
		nufs_put32(v, raw, DI_SIZE + 4, (uint32_t)(dp->size >> 32));
	}
	nufs_put32(v, raw, DI_ATIME, dp->atime);
	nufs_put32(v, raw, DI_MTIME, dp->mtime);
	nufs_put32(v, raw, DI_CTIME, dp->ctime);
	if (dp->flags & NUFS_IC_FASTLINK)
		memcpy(raw + DI_SYMLINK, dp->symlink, NUFS_MAXFASTLINK);
	else {
		for (i = 0; i < NUFS_NDADDR; i++)
			nufs_put32(v, raw, DI_DB + 4 * i, (uint32_t)dp->db[i]);
		for (i = 0; i < NUFS_NIADDR; i++)
			nufs_put32(v, raw, DI_IB + 4 * i, (uint32_t)dp->ib[i]);
	}
	nufs_put32(v, raw, DI_FLAGS, dp->flags);
	nufs_put32(v, raw, DI_BLOCKS, dp->blocks);
	nufs_put32(v, raw, DI_GEN, dp->gen);
	return nufs_pwrite(v, raw, inode_offset(v, dp->ino), sizeof(raw));
}

/* Map a logical block number to its fragment address; 0 means a hole. */
int
nufs_bmap(struct nufs *v, const struct nufs_dinode *dp, int lbn, int *frag)
{
	int nindir = v->nindir, level, i;
	int idx[NUFS_NIADDR], nlevels;
	int32_t blk;
	uint8_t *buf;

	if (lbn < 0) {
		nufs_err(v, "negative logical block");
		return -1;
	}
	if (lbn < NUFS_NDADDR) {
		*frag = dp->db[lbn];
		return 0;
	}
	lbn -= NUFS_NDADDR;
	for (nlevels = 1; nlevels <= NUFS_NIADDR; nlevels++) {
		long long span = 1;

		for (i = 0; i < nlevels; i++)
			span *= nindir;
		if (lbn < span)
			break;
		lbn -= (int)span;
	}
	if (nlevels > NUFS_NIADDR) {
		nufs_err(v, "logical block beyond triple indirect");
		return -1;
	}
	for (level = nlevels - 1, i = 0; level >= 0; level--, i++) {
		long long span = 1;
		int j;

		for (j = 0; j < level; j++)
			span *= nindir;
		idx[i] = (int)(lbn / span);
		lbn = (int)(lbn % span);
	}

	blk = dp->ib[nlevels - 1];
	buf = malloc((size_t)v->bsize);
	if (buf == NULL) {
		nufs_err(v, "out of memory");
		return -1;
	}
	for (i = 0; i < nlevels; i++) {
		if (blk == 0) {
			free(buf);
			*frag = 0;
			return 0;
		}
		if (nufs_read_frags(v, blk, v->frag, buf) != 0) {
			free(buf);
			return -1;
		}
		blk = (int32_t)nufs_get32(v, buf, 4 * idx[i]);
	}
	free(buf);
	*frag = blk;
	return 0;
}

long
nufs_file_read(struct nufs *v, const struct nufs_dinode *dp, void *vbuf,
    long long off, long n)
{
	uint8_t *out = vbuf, *blk;
	long done = 0;

	if (off >= (long long)dp->size)
		return 0;
	if (off + n > (long long)dp->size)
		n = (long)(dp->size - off);
	blk = malloc((size_t)v->bsize);
	if (blk == NULL) {
		nufs_err(v, "out of memory");
		return -1;
	}
	while (done < n) {
		long long pos = off + done;
		int lbn = (int)(pos / v->bsize);
		int boff = (int)(pos % v->bsize);
		long chunk = v->bsize - boff;
		int frag, nfrags;

		if (chunk > n - done)
			chunk = n - done;
		if (nufs_bmap(v, dp, lbn, &frag) != 0) {
			free(blk);
			return -1;
		}
		if (frag == 0)
			memset(out + done, 0, (size_t)chunk);
		else {
			/*
			 * The tail of a file can be a partial block holding
			 * only as many frags as it needs.
			 */
			nfrags = nufs_lbn_frags(v, dp->size, lbn);
			memset(blk, 0, (size_t)v->bsize);
			if (nufs_read_frags(v, frag, nfrags, blk) != 0) {
				free(blk);
				return -1;
			}
			memcpy(out + done, blk + boff, (size_t)chunk);
		}
		done += chunk;
	}
	free(blk);
	return done;
}

int
nufs_readlink(struct nufs *v, const struct nufs_dinode *dp, char *buf, size_t n)
{
	long got;

	if (dp->size >= n) {
		nufs_err(v, "symlink target too long");
		return -1;
	}
	if (dp->flags & NUFS_IC_FASTLINK) {
		memcpy(buf, dp->symlink, (size_t)dp->size);
		buf[dp->size] = '\0';
		return 0;
	}
	got = nufs_file_read(v, dp, buf, 0, (long)dp->size);
	if (got < 0)
		return -1;
	buf[got] = '\0';
	return 0;
}
