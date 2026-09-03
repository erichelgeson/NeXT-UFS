#include "nextufs.h"
#include <errno.h>
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

/*
 * A Macintosh OSType, four printable bytes. Anything else is A/UX using the
 * field for something we do not model, and is reported as absent.
 */
static void
ostype(char *dst, const uint8_t *src)
{
	int i;

	for (i = 0; i < 4; i++)
		if (src[i] < 0x20 || src[i] >= 0x7f) {
			dst[0] = '\0';
			return;
		}
	memcpy(dst, src, 4);
	dst[4] = '\0';
}

/*
 * The two halves of the quad di_size, in the order the volume stores them.
 * Either may be NULL when only the other is wanted.
 */
static void
size_halves(const struct nufs *v, const uint8_t *raw, uint32_t *hi, uint32_t *lo)
{
	if (hi != NULL)
		*hi = nufs_get32(v, raw, v->be ? DI_SIZE : DI_SIZE + 4);
	if (lo != NULL)
		*lo = nufs_get32(v, raw, v->be ? DI_SIZE + 4 : DI_SIZE);
}

/*
 * A/UX puts a directory's valence -- its entry count, less "." and ".." --
 * in the high half of di_size, which 4.3BSD and NeXT always leave zero, and
 * a Macintosh directory ID in the word 4.3BSD calls di_flags. An Apple
 * Partition Map settles the question on its own. A bare slice has to be read,
 * and the root inode carries both marks: a valence, and the directory ID 2,
 * which is what HFS calls fsRtDirID.
 *
 * Either one alone is enough, because each has a case the other misses. A
 * volume A/UX has installed but whose Finder has never opened the root has
 * no valence there. The directory ID would be missing the same way, but 2 is
 * fixed for the root, so in practice it is the one that survives. NeXT
 * leaves both words zero, and its only use of that second word, the inline
 * symlink bit, cannot appear on a directory.
 */
int
nufs_detect_aux(struct nufs *v)
{
	uint8_t raw[NUFS_DINODE_SIZE];
	uint32_t hi;

	if (v->apm.valid)
		return 1;
	if (nufs_pread(v, raw, inode_offset(v, NUFS_ROOTINO), sizeof(raw)) != 0) {
		v->err[0] = '\0';
		return 0;
	}
	if ((nufs_get16(v, raw, DI_MODE) & 0170000) != 0040000)
		return 0;
	size_halves(v, raw, &hi, NULL);
	return hi != 0 || nufs_get32(v, raw, DI_AUXID) == NUFS_MACROOTID;
}

/*
 * Wipe an inode on disk. nufs_inode_write deliberately preserves every word
 * it does not model, which is right for an inode in use and wrong for one
 * just handed out: without this a new file inherits the Finder type, dates
 * and fork lengths of whatever used the number last. 4.3BSD's ialloc clears
 * the inode for the same reason.
 */
int
nufs_inode_clear(struct nufs *v, uint32_t ino)
{
	uint8_t zero[NUFS_DINODE_SIZE];

	memset(zero, 0, sizeof(zero));
	return nufs_pwrite(v, zero, inode_offset(v, ino), sizeof(zero));
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
	{
		uint32_t hi, lo;

		size_halves(v, raw, &hi, &lo);
		if (v->aux) {
			dp->size = lo;
			dp->valence = hi;
			ostype(dp->fdtype, raw + DI_FDTYPE);
			ostype(dp->fdcreator, raw + DI_FDCREATOR);
			dp->fdlen = nufs_get32(v, raw, DI_FDLEN);
			dp->fdcrdat = nufs_get32(v, raw, DI_FDCRDAT);
			dp->fdflags = nufs_get16(v, raw, DI_FDFLAGS);
		} else
			dp->size = ((uint64_t)hi << 32) | lo;
	}
	dp->atime = nufs_get32(v, raw, DI_ATIME);
	dp->mtime = nufs_get32(v, raw, DI_MTIME);
	dp->ctime = nufs_get32(v, raw, DI_CTIME);
	if (v->dyncg) {			/* SunOS timevals; see set_time() */
		dp->ausec = nufs_get32(v, raw, DI_ATIME + 4);
		dp->musec = nufs_get32(v, raw, DI_MTIME + 4);
		dp->cusec = nufs_get32(v, raw, DI_CTIME + 4);
	}
	/*
	 * A/UX uses this word for Macintosh bookkeeping, so di_flags does not
	 * apply and IC_FASTLINK must not be read out of it: real directories
	 * have the bit set, and honouring it would put a symlink target over
	 * their block list.
	 */
	if (v->aux)
		dp->auxid = nufs_get32(v, raw, DI_AUXID);
	else
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

/*
 * A/UX keeps the Finder's own copy of a file's modification date and data
 * fork length in two of the spare longs, and the Finder shows those rather
 * than what UNIX holds. Rewriting a file without them leaves the Finder
 * describing a file that no longer exists.
 *
 * Two rules stop this inventing metadata. A file whose fdMdDat is zero has
 * never been shown in the Finder, since A/UX fills these in lazily, so it is
 * left alone the way a new file gets no type or creator. And fdLen is only
 * followed when it already agreed with the size. That is what separates a
 * plain file from one held as AppleSingle, where the UNIX file carries a
 * header and both forks and fdLen is rightly smaller; guessing there would
 * turn a stale value into a wrong one.
 *
 * A directory's two words are the Macintosh directory ID and frMdDat.
 * Neither follows from anything here, so a directory is left untouched.
 */
static void
aux_finder_update(const struct nufs *v, uint8_t *raw,
    const struct nufs_dinode *dp, uint32_t oldsize)
{
	if ((dp->mode & 0170000) != 0100000)
		return;
	if (nufs_get32(v, raw, DI_FDMDDAT) == 0)
		return;
	if (nufs_get32(v, raw, DI_FDLEN) == oldsize)
		nufs_put32(v, raw, DI_FDLEN, (uint32_t)dp->size);
	nufs_put32(v, raw, DI_FDMDDAT, dp->mtime);
}

static void
set_time(const struct nufs *v, uint8_t *raw, int off, uint32_t sec)
{
	if (v->dyncg && nufs_get32(v, raw, off) != sec)
		nufs_put32(v, raw, off + 4, 0);
	nufs_put32(v, raw, off, sec);
}

int
nufs_inode_write(struct nufs *v, const struct nufs_dinode *dp)
{
	uint8_t raw[NUFS_DINODE_SIZE];
	uint32_t oldsize;
	int i;

	/* Preserve the spare words and anything else we do not model. */
	if (nufs_pread(v, raw, inode_offset(v, dp->ino), sizeof(raw)) != 0)
		return -1;
	size_halves(v, raw, NULL, &oldsize);

	nufs_put16(v, raw, DI_MODE, dp->mode);
	nufs_put16(v, raw, DI_NLINK, (uint16_t)dp->nlink);
	nufs_put16(v, raw, DI_UID, dp->uid);
	nufs_put16(v, raw, DI_GID, dp->gid);
	{
		uint32_t hi = v->aux ? dp->valence : (uint32_t)(dp->size >> 32);
		uint32_t lo = (uint32_t)dp->size;

		if (v->be) {
			nufs_put32(v, raw, DI_SIZE, hi);
			nufs_put32(v, raw, DI_SIZE + 4, lo);
		} else {
			nufs_put32(v, raw, DI_SIZE, lo);
			nufs_put32(v, raw, DI_SIZE + 4, hi);
		}
	}
	/*
	 * Each timestamp is followed by a word all three filesystems use
	 * differently. NeXT leaves it zero, A/UX keeps Finder data in it, and
	 * SunOS stores a whole struct timeval, so the word is the microseconds
	 * of the second before it. Setting a SunOS time to a new second leaves
	 * a fraction belonging to the old one, so clear it -- and only then,
	 * or an untouched timestamp would lose the fraction it came with.
	 */
	set_time(v, raw, DI_ATIME, dp->atime);
	set_time(v, raw, DI_MTIME, dp->mtime);
	set_time(v, raw, DI_CTIME, dp->ctime);
	if (dp->flags & NUFS_IC_FASTLINK)
		memcpy(raw + DI_SYMLINK, dp->symlink, NUFS_MAXFASTLINK);
	else {
		for (i = 0; i < NUFS_NDADDR; i++)
			nufs_put32(v, raw, DI_DB + 4 * i, (uint32_t)dp->db[i]);
		for (i = 0; i < NUFS_NIADDR; i++)
			nufs_put32(v, raw, DI_IB + 4 * i, (uint32_t)dp->ib[i]);
	}
	if (v->aux)
		aux_finder_update(v, raw, dp, oldsize);
	else
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
		nufs_errc(v, EFBIG, "logical block beyond triple indirect");
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
		nufs_errc(v, ENOMEM, "out of memory");
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
		nufs_errc(v, ENOMEM, "out of memory");
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
		nufs_errc(v, ENAMETOOLONG, "symlink target too long");
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
