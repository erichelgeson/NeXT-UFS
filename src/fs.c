#include "nextufs.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define SBGET(f)	((int32_t)nufs_get32(v, v->sb, (f)))

static int
read_superblock(struct nufs *v)
{
	uint8_t probe[4];

	if (nufs_pread(v, probe, v->partoff + NUFS_SBOFF + FS_MAGIC, 4) != 0)
		return -1;
	if (probe[0] == 0x00 && probe[1] == 0x01 && probe[2] == 0x19 &&
	    probe[3] == 0x54)
		v->be = 1;
	else if (probe[3] == 0x00 && probe[2] == 0x01 && probe[1] == 0x19 &&
	    probe[0] == 0x54)
		v->be = 0;
	else {
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
	if ((int32_t)nufs_get32(v, v->cgbuf, CG_MAGIC) != NUFS_CG_MAGIC) {
		nufs_err(v, "cylinder group %d has a bad magic number", cg);
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

/* No label: hunt for a superblock on a sector boundary. */
static int
scan_for_fs(struct nufs *v)
{
	static const long long step = 512;
	long long off;
	uint8_t probe[4];

	for (off = 0; off < 64LL << 20; off += step) {
		if (nufs_pread(v, probe, off + NUFS_SBOFF + FS_MAGIC, 4) != 0)
			break;
		if ((probe[0] == 0x00 && probe[1] == 0x01 && probe[2] == 0x19 &&
		    probe[3] == 0x54) ||
		    (probe[3] == 0x00 && probe[2] == 0x01 && probe[1] == 0x19 &&
		    probe[0] == 0x54)) {
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
	v->be = 1;
	v->rw = rw;
	v->path = strdup(path);
	v->f = fopen(path, rw ? "r+b" : "rb");
	if (v->f == NULL) {
		nufs_err(v, "cannot open %s", path);
		goto fail;
	}
	if (nufs_label_read(v, &v->label) == 0) {
		int p = pick_partition(v, partspec);

		if (p < 0)
			goto fail;
		v->partno = p;
		v->partoff = ((long long)v->label.front + v->label.part[p].base) *
		    v->label.secsize;
	} else if (partspec != NULL && partspec[0] != '\0') {
		nufs_err(v, "no NeXT disk label, so partition '%s' means nothing",
		    partspec);
		goto fail;
	} else if (scan_for_fs(v) != 0)
		goto fail;

	if (read_superblock(v) != 0)
		goto fail;

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
