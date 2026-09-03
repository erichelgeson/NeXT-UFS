#include "nextufs.h"
#include <errno.h>
#include <sys/file.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SBGET(f)	((int32_t)nufs_get32(v, v->sb, (f)))

/*
 * Is there a superblock for a partition starting at `off`? Sets *be to the
 * volume's byte order. Returns 0 on a hit, -1 for no magic there, and -2 if
 * the read ran off the end of the image.
 */
static int
sb_magic_at(struct nufs *v, long long off, int *be)
{
	uint8_t probe[4];

	if (nufs_pread(v, probe, off + NUFS_SBOFF + FS_MAGIC, 4) != 0)
		return -2;
	if (probe[0] == 0x00 && probe[1] == 0x01 && probe[2] == 0x19 &&
	    probe[3] == 0x54)
		*be = 1;
	else if (probe[3] == 0x00 && probe[2] == 0x01 && probe[1] == 0x19 &&
	    probe[0] == 0x54)
		*be = 0;
	else
		return -1;
	return 0;
}

static int
read_superblock(struct nufs *v)
{
	if (sb_magic_at(v, v->partoff, &v->be) != 0) {
		nufs_err(v, "no UFS superblock at offset %lld",
		    v->partoff + NUFS_SBOFF);
		return -1;
	}
	if (nufs_pread(v, v->sb, v->partoff + NUFS_SBOFF, 2048) != 0)
		return -1;

	v->sblkno = SBGET(FS_SBLKNO);
	v->cblkno = SBGET(FS_CBLKNO);
	v->iblkno = SBGET(FS_IBLKNO);
	v->dblkno = SBGET(FS_DBLKNO);
	v->cgoffset = SBGET(FS_CGOFFSET);
	v->cgmask = SBGET(FS_CGMASK);
	v->size = SBGET(FS_SIZE);
	v->dsize = SBGET(FS_DSIZE);
	v->ncg = SBGET(FS_NCG);
	v->bsize = SBGET(FS_BSIZE);
	v->fsize = SBGET(FS_FSIZE);
	v->frag = SBGET(FS_FRAG);
	v->minfree = SBGET(FS_MINFREE);
	v->bshift = SBGET(FS_BSHIFT);
	v->fshift = SBGET(FS_FSHIFT);
	v->fragshift = SBGET(FS_FRAGSHIFT);
	v->fsbtodb = SBGET(FS_FSBTODB);
	v->sbsize = SBGET(FS_SBSIZE);
	v->csmask = SBGET(FS_CSMASK);
	v->csshift = SBGET(FS_CSSHIFT);
	v->nindir = SBGET(FS_NINDIR);
	v->inopb = SBGET(FS_INOPB);
	v->nspf = SBGET(FS_NSPF);
	v->optim = SBGET(FS_OPTIM);
	v->npsect = SBGET(FS_NPSECT);
	v->interleave = SBGET(FS_INTERLEAVE);
	v->trackskew = SBGET(FS_TRACKSKEW);
	v->csaddr = SBGET(FS_CSADDR);
	v->cssize = SBGET(FS_CSSIZE);
	v->cgsize = SBGET(FS_CGSIZE);
	v->ntrak = SBGET(FS_NTRAK);
	v->nsect = SBGET(FS_NSECT);
	v->spc = SBGET(FS_SPC);
	v->ncyl = SBGET(FS_NCYL);
	v->cpg = SBGET(FS_CPG);
	v->ipg = SBGET(FS_IPG);
	v->fpg = SBGET(FS_FPG);
	v->cpc = SBGET(FS_CPC);

	if (v->bsize <= 0 || v->fsize <= 0 || v->frag <= 0 || v->ncg <= 0 ||
	    v->ipg <= 0 || v->fpg <= 0 || v->inopb <= 0 ||
	    v->bsize != v->fsize * v->frag) {
		nufs_err(v, "superblock geometry is not sane");
		return -1;
	}
	/*
	 * A directory is a run of device blocks, and no entry may straddle one.
	 * That is DEV_BSIZE, which the geometry gives up as fs_fsize shifted by
	 * fs_fsbtodb: 1024 on NeXT hardware, 512 on A/UX.
	 */
	v->dirblksiz = v->fsize >> (v->fsbtodb < 0 || v->fsbtodb > 3 ? 0 :
	    v->fsbtodb);
	if (v->dirblksiz < 512 || v->dirblksiz > NUFS_DIRBLKSIZ)
		v->dirblksiz = NUFS_DIRBLKSIZ;
	if (v->cgsize > NUFS_SBSIZE || v->cgsize <= 0) {
		nufs_err(v, "implausible cylinder group size %d", v->cgsize);
		return -1;
	}
	return 0;
}

long long
nufs_fragoff(const struct nufs *v, int frag)
{
	return v->partoff + (long long)frag * v->fsize;
}

int
nufs_read_frags(struct nufs *v, int frag, int nfrags, void *buf)
{
	if (frag < 0 || frag + nfrags > v->size) {
		nufs_err(v, "frag %d out of range (fs has %d)", frag, v->size);
		return -1;
	}
	return nufs_pread(v, buf, nufs_fragoff(v, frag),
	    (size_t)nfrags * v->fsize);
}

int
nufs_write_frags(struct nufs *v, int frag, int nfrags, const void *buf)
{
	if (frag < 0 || frag + nfrags > v->size) {
		nufs_err(v, "frag %d out of range (fs has %d)", frag, v->size);
		return -1;
	}
	return nufs_pwrite(v, buf, nufs_fragoff(v, frag),
	    (size_t)nfrags * v->fsize);
}

int
nufs_cgstart(const struct nufs *v, int c)
{
	return v->fpg * c + v->cgoffset * (c & ~v->cgmask);
}

/*
 * A dynamic cylinder group says where its own four maps are. Take them from
 * the group in the buffer, and insist they run in the order the layout puts
 * them in and end inside the group: everything downstream indexes straight
 * into cgbuf, so a group with a wild offset in it must not get that far.
 */
static int
cg_map_offsets(struct nufs *v, int cg)
{
	int btot = (int32_t)nufs_get32(v, v->cgbuf, CG_DYN_BTOTOFF);
	int b = (int32_t)nufs_get32(v, v->cgbuf, CG_DYN_BOFF);
	int iused = (int32_t)nufs_get32(v, v->cgbuf, CG_DYN_IUSEDOFF);
	int free = (int32_t)nufs_get32(v, v->cgbuf, CG_DYN_FREEOFF);
	int end = (int32_t)nufs_get32(v, v->cgbuf, CG_DYN_NEXTFREEOFF);
	int cpg = (b - btot) / 4;

	if (btot < CG_DYN_NEXTFREEOFF + 4 || btot > b || b > iused ||
	    iused > free || free > end || end > v->cgsize ||
	    b - btot != cpg * 4 || iused - b < cpg * v->nrpos * 2 ||
	    free - iused < (v->ipg + 7) / 8 || end - free < (v->fpg + 7) / 8) {
		nufs_err(v, "cylinder group %d has an unusable map layout", cg);
		return -1;
	}
	v->cg_btot = btot;
	v->cg_b = b;
	v->cg_iused = iused;
	v->cg_free = free;
	v->cgcpg = cpg;
	return 0;
}

int
nufs_cg_load(struct nufs *v, int cg)
{
	int base;

	if (cg < 0 || cg >= v->ncg) {
		nufs_err(v, "cylinder group %d out of range", cg);
		return -1;
	}
	if (v->cgno == cg)
		return 0;
	if (nufs_flush(v) != 0)
		return -1;
	base = nufs_cgstart(v, cg) + v->cblkno;
	if (nufs_pread(v, v->cgbuf, nufs_fragoff(v, base), v->cgsize) != 0)
		return -1;
	if ((int32_t)nufs_get32(v, v->cgbuf, v->dyncg ? CG_DYN_MAGIC :
	    CG_MAGIC) != NUFS_CG_MAGIC) {
		nufs_err(v, "cylinder group %d has a bad magic number", cg);
		v->cgno = -1;
		return -1;
	}
	if (v->dyncg && cg_map_offsets(v, cg) != 0) {
		v->cgno = -1;
		return -1;
	}
	v->cgno = cg;
	return 0;
}

int
nufs_flush(struct nufs *v)
{
	if (!v->rw)
		return 0;
	if (v->dirty_cg && v->cgno >= 0) {
		int base = nufs_cgstart(v, v->cgno) + v->cblkno;

		if (nufs_pwrite(v, v->cgbuf, nufs_fragoff(v, base),
		    v->cgsize) != 0)
			return -1;
		v->dirty_cg = 0;
	}
	if (v->dirty_csum) {
		if (nufs_pwrite(v, v->csum, nufs_fragoff(v, v->csaddr),
		    v->cssize) != 0)
			return -1;
		v->dirty_csum = 0;
	}
	if (v->dirty_sb) {
		if (nufs_pwrite(v, v->sb, v->partoff + NUFS_SBOFF, 2048) != 0)
			return -1;
		v->dirty_sb = 0;
	}
	fflush(v->f);
	return 0;
}

/*
 * Record whether the volume is being written. NeXT's kernel refuses to mount
 * a volume whose fs_state is not NUFS_STATE_CLEAN without running fsck first,
 * which is exactly what should happen if a mount is killed mid-write.
 */
int
nufs_mark(struct nufs *v, int state)
{
	if (!v->rw)
		return 0;
	/*
	 * fs_state is NeXT's. A/UX leaves the byte zero and has no equivalent,
	 * so there is nothing to record and nothing worth inventing.
	 */
	if (v->aux)
		return 0;
	nufs_put32(v, v->sb, FS_TIME, (uint32_t)time(NULL));
	if (v->dyncg) {
		/*
		 * SunOS puts FSCLEAN in the same byte but counts the other
		 * way, and only believes it while fs_state holds FSOKAY minus
		 * the write time. Leaving fs_state stale is how the volume
		 * gets checked after a crash, so a dirty mark is the word
		 * zeroed rather than a value of its own.
		 */
		v->sb[FS_FMOD] = (uint8_t)(state == NUFS_STATE_DIRTY);
		v->sb[FS_STATE] = (uint8_t)(state == NUFS_STATE_CLEAN ?
		    NUFS_FSCLEAN : NUFS_FSACTIVE);
		nufs_put32(v, v->sb, FS_SUN_STATE, state == NUFS_STATE_CLEAN ?
		    (uint32_t)NUFS_FSOKAY - nufs_get32(v, v->sb, FS_TIME) : 0);
	} else {
		v->sb[FS_STATE] = (uint8_t)state;
		v->sb[FS_FMOD] = (uint8_t)(state == NUFS_STATE_DIRTY);
	}
	v->dirty_sb = 1;
	return nufs_flush(v);
}

/*
 * Is the volume clean? Three filesystems, three answers: NeXT's fs_state
 * byte, SunOS's fs_clean byte vouched for by the fs_state word, and A/UX,
 * which records nothing at all and is reported clean because there is
 * nothing else it could be.
 */
int
nufs_is_clean(const struct nufs *v)
{
	if (v->aux)
		return 1;
	if (!v->dyncg)
		return v->sb[FS_STATE] == NUFS_STATE_CLEAN;
	if (nufs_get32(v, v->sb, FS_SUN_STATE) !=
	    (uint32_t)NUFS_FSOKAY - nufs_get32(v, v->sb, FS_TIME))
		return 0;
	return v->sb[FS_STATE] == NUFS_FSCLEAN ||
	    v->sb[FS_STATE] == NUFS_FSSTABLE;
}

/* Flush the buffers this library holds, then the ones the kernel holds. */
int
nufs_sync(struct nufs *v)
{
	if (nufs_flush(v) != 0)
		return -1;
	if (v->rw && fsync(fileno(v->f)) != 0) {
		nufs_err(v, "fsync failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Which cylinder group layout is this? NeXT and A/UX use the original 4.2BSD
 * one, with the maps at fixed offsets and cg_magic at 980. SunOS and every
 * later BSD use the dynamic one, which moves cg_magic to the front and puts
 * the maps wherever the header says. fs_postblformat is supposed to say so,
 * but the old layout has rotation data in those bytes, so the group itself is
 * what settles it. SunOS's own cg_chkmagic accepts either, the same way.
 *
 * The static map offsets set here are what a NeXT or A/UX volume uses for
 * good; a dynamic one has them replaced per group by cg_map_offsets().
 */
static int
detect_cgfmt(struct nufs *v)
{
	uint8_t *cg = malloc((size_t)v->cgsize);
	int rc = 0;

	v->cg_btot = CG_BTOT;
	v->cg_b = CG_B;
	v->cg_iused = CG_IUSED;
	v->cg_free = CG_FREE;
	v->cgcpg = NUFS_MAXCPG;
	v->nrpos = NUFS_NRPOS;

	if (cg == NULL) {
		nufs_errc(v, ENOMEM, "out of memory");
		return -1;
	}
	if (nufs_pread(v, cg, nufs_fragoff(v, nufs_cgstart(v, 0) + v->cblkno),
	    (size_t)v->cgsize) != 0)
		rc = -1;
	else if ((int32_t)nufs_get32(v, cg, CG_MAGIC) == NUFS_CG_MAGIC)
		v->dyncg = 0;
	else if ((int32_t)nufs_get32(v, cg, CG_DYN_MAGIC) == NUFS_CG_MAGIC)
		v->dyncg = 1;
	else
		v->dyncg = 0;		/* damaged; leave it for fsck to say */
	free(cg);

	if (v->dyncg) {
		v->nrpos = (int32_t)nufs_get32(v, v->sb, FS_NRPOS);
		if (v->nrpos < 1 || v->nrpos > 1024) {
			nufs_err(v, "fs_nrpos is %d, which cannot be right",
			    v->nrpos);
			return -1;
		}
	}
	return rc;
}

static int
pick_partition(struct nufs *v, const char *spec)
{
	const struct nufs_label *l = &v->label;
	int i;

	if (spec != NULL && spec[0] != '\0') {
		if (spec[0] >= 'a' && spec[0] < 'a' + NUFS_NPART && spec[1] == '\0')
			return spec[0] - 'a';
		nufs_err(v, "bad partition '%s' (want a..h)", spec);
		return -1;
	}
	for (i = 0; i < NUFS_NPART; i++)
		if (l->part[i].size > 0 && l->part[i].base >= 0 &&
		    strcmp(l->part[i].type, "4.3BSD") == 0)
			return i;
	for (i = 0; i < NUFS_NPART; i++)
		if (l->part[i].size > 0 && l->part[i].base >= 0)
			return i;
	nufs_err(v, "no usable partition in the label");
	return -1;
}

/*
 * An A/UX disk. `spec` is a map entry number as Apple counts them, 1-based,
 * so partition 5 of this map is what A/UX calls /dev/dsk/c0d0s5; a partition
 * name works too. With no spec, take the largest UNIX slice that actually
 * has a superblock: a disk normally has three, and the other two are swap
 * and the tiny Eschatology crash-recovery area.
 */
static int
pick_apm_partition(struct nufs *v, const char *spec)
{
	const struct nufs_apm *m = &v->apm;
	int i, best = -1, be;

	if (spec != NULL && spec[0] != '\0') {
		char *end;
		long n = strtol(spec, &end, 10);

		if (*end == '\0' && n >= 1 && n <= m->n)
			best = (int)n - 1;
		else
			for (i = 0; i < m->n; i++)
				if (strcmp(m->part[i].name, spec) == 0) {
					best = i;
					break;
				}
		if (best < 0) {
			nufs_err(v, "no partition '%s' in the Apple Partition"
			    " Map (it has %d)", spec, m->n);
			return -1;
		}
		return best;
	}
	for (i = 0; i < m->n; i++) {
		const struct nufs_apm_part *p = &m->part[i];

		if (!nufs_apm_is_unix(p))
			continue;
		if (sb_magic_at(v, (long long)p->start * m->blocksize, &be) != 0)
			continue;
		if (best < 0 || p->size > m->part[best].size)
			best = i;
	}
	if (best < 0)
		nufs_err(v, "no UFS partition in the Apple Partition Map");
	return best;
}

/* No label and no map: hunt for a superblock on a sector boundary. */
static int
scan_for_fs(struct nufs *v)
{
	static const long long step = 512;
	long long off;
	int be;

	for (off = 0; off < 64LL << 20; off += step) {
		int rc = sb_magic_at(v, off, &be);

		if (rc == -2)
			break;
		if (rc == 0) {
			v->partoff = off;
			return 0;
		}
	}
	nufs_err(v, "no NeXT disk label and no superblock found");
	return -1;
}

struct nufs *
nufs_open(const char *path, const char *partspec, int rw, char *errbuf,
    size_t errlen)
{
	struct nufs *v;

	v = calloc(1, sizeof(*v));
	if (v == NULL)
		goto fail;
	v->cgno = -1;
	v->partno = -1;
	v->apmno = -1;
	v->be = 1;
	v->rw = rw;
	v->path = strdup(path);
	v->f = fopen(path, rw ? "r+b" : "rb");
	if (v->f == NULL) {
		nufs_err(v, "cannot open %s", path);
		goto fail;
	}
	/*
	 * One writer at a time: the CLI must not scribble on a mounted image,
	 * and an image must not be mounted twice.
	 */
	if (flock(fileno(v->f), (rw ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0 &&
	    (errno == EWOULDBLOCK || errno == EAGAIN)) {
		nufs_errc(v, EBUSY, "%s is in use (mounted, or open elsewhere)",
		    path);
		goto fail;
	}
	if (nufs_label_read(v, &v->label) == 0) {
		int p = pick_partition(v, partspec);

		if (p < 0)
			goto fail;
		v->partno = p;
		v->partoff = ((long long)v->label.front + v->label.part[p].base) *
		    v->label.secsize;
	} else if (nufs_apm_read(v, &v->apm) == 0) {
		int p = pick_apm_partition(v, partspec);

		if (p < 0)
			goto fail;
		v->apmno = p;
		v->partoff = (long long)v->apm.part[p].start * v->apm.blocksize;
	} else if (partspec != NULL && partspec[0] != '\0') {
		nufs_err(v, "no partition table, so partition '%s' means nothing",
		    partspec);
		goto fail;
	} else if (scan_for_fs(v) != 0)
		goto fail;

	if (read_superblock(v) != 0)
		goto fail;
	if (detect_cgfmt(v) != 0)
		goto fail;
	v->aux = nufs_detect_aux(v);

	v->cgbuf = malloc((size_t)v->cgsize);
	v->csum = malloc((size_t)v->cssize);
	if (v->cgbuf == NULL || v->csum == NULL) {
		nufs_errc(v, ENOMEM, "out of memory");
		goto fail;
	}
	if (nufs_pread(v, v->csum, nufs_fragoff(v, v->csaddr),
	    (size_t)v->cssize) != 0)
		goto fail;
	return v;

fail:
	if (v != NULL) {
		if (errbuf != NULL)
			snprintf(errbuf, errlen, "%s", v->err[0] ? v->err :
			    "out of memory");
		nufs_close(v);
	} else if (errbuf != NULL)
		snprintf(errbuf, errlen, "out of memory");
	return NULL;
}

void
nufs_close(struct nufs *v)
{
	if (v == NULL)
		return;
	if (v->rw)
		nufs_flush(v);
	if (v->f != NULL)
		fclose(v->f);
	free(v->path);
	free(v->cgbuf);
	free(v->csum);
	free(v);
}
