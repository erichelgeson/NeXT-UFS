/*
 * Create a NeXT disk label and an empty 4.3BSD filesystem in partition a.
 *
 * Layout follows 4.3BSD newfs, with the values NeXT's own newfs uses and that
 * both reference volumes carry: 8KB blocks, 1KB fragments, fs_sblkno 16,
 * fs_cblkno 24, fs_iblkno 32, a 160-sector front porch holding four label
 * copies, and the cylinder group staggering fs_cgoffset 16 / fs_cgmask ~3.
 *
 * The root directory and lost+found are built afterwards through the ordinary
 * allocator, so a fresh volume is made by exactly the code that maintains an
 * existing one.
 */
#include "nextufs.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOWMANY(x, y)	(((x) + (y) - 1) / (y))
#define ROUNDUP(x, y)	(HOWMANY(x, y) * (y))

#define SECSIZE		1024
#define FRONT_PORCH	160		/* sectors before partition a */
#define LABEL_STRIDE	0x1e00		/* byte spacing of the four labels */
#define NLABELS		4
#define BSIZE		8192
#define FSIZE		1024
#define FRAGS		(BSIZE / FSIZE)
#define NTRAK		4
#define NSECT		64
#define CPG		16
#define DENSITY		4096		/* bytes per inode */
#define MINFREE		5
#define RPM		3600

struct mkfs {
	int	secsize, ntrak, nsect, spc, cpg, ncyl, ncg;
	int	fsize, bsize, frag, ipg, fpg, inopb, nindir;
	int	sblkno, cblkno, iblkno, dblkno, cgoffset, cgmask;
	int	csaddr, cssize, cgsize, cpc;
	int	fssize;				/* fragments in the partition */
	long long partoff;
};

static void
put32(uint8_t *p, int off, uint32_t v)
{
	p[off] = (uint8_t)(v >> 24);
	p[off + 1] = (uint8_t)(v >> 16);
	p[off + 2] = (uint8_t)(v >> 8);
	p[off + 3] = (uint8_t)v;
}

static void
put16(uint8_t *p, int off, uint16_t v)
{
	p[off] = (uint8_t)(v >> 8);
	p[off + 1] = (uint8_t)v;
}

static int
cgstart_of(const struct mkfs *m, int c)
{
	return m->fpg * c + m->cgoffset * (c & ~m->cgmask);
}

static int
cbtocylno(const struct mkfs *m, int bno)
{
	return bno / m->spc;			/* fs_nspf is 1 on NeXT */
}

static int
cbtorpos(const struct mkfs *m, int bno)
{
	return bno % m->spc % m->nsect * NUFS_NRPOS / m->nsect;
}

/* --- the disk label ------------------------------------------------------ */

static void
build_label(const struct mkfs *m, uint8_t *lab, const char *name, int totsect)
{
	int o, i;

	memset(lab, 0, NUFS_SBSIZE);
	memcpy(lab + DL_VERSION, "dlV3", 4);
	put32(lab, DL_LABEL_BLKNO, 0);
	put32(lab, DL_SIZE, (uint32_t)totsect);
	snprintf((char *)lab + DL_LABEL, 24, "%s", name);
	put32(lab, DL_FLAGS, 0);
	put32(lab, DL_TAG, 0);

	snprintf((char *)lab + DT_NAME, 24, "nextufs");
	snprintf((char *)lab + DT_TYPE, 24, "fixed_rw_scsi");
	put32(lab, DT_SECSIZE, (uint32_t)m->secsize);
	put32(lab, DT_NTRACKS, (uint32_t)m->ntrak);
	put32(lab, DT_NSECTORS, (uint32_t)m->nsect);
	put32(lab, DT_NCYLINDERS, (uint32_t)(totsect / m->spc));
	put32(lab, DT_RPM, RPM);
	put16(lab, DT_FRONT, FRONT_PORCH);
	put16(lab, DT_BACK, 0);
	put16(lab, DT_NGROUPS, 0);
	put16(lab, DT_AG_SIZE, 0);
	put16(lab, DT_AG_ALTS, 0);
	put16(lab, DT_AG_OFF, 0);
	put32(lab, DT_BOOT0_BLKNO, 32);
	put32(lab, DT_BOOT0_BLKNO + 4, 96);
	snprintf((char *)lab + DT_BOOTFILE, 24, "sdmach");
	snprintf((char *)lab + DT_HOSTNAME, 32, "nextufs");
	lab[DT_ROOTPART] = 'a';
	lab[DT_RWPART] = 'b';

	for (i = 0; i < NUFS_NPART; i++) {
		o = DT_PARTITIONS + i * NUFS_PARTSIZE;
		memset(lab + o, 0xff, NUFS_PARTSIZE);
	}
	o = DT_PARTITIONS;
	memset(lab + o, 0, NUFS_PARTSIZE);
	put32(lab, o + P_BASE, 0);
	put32(lab, o + P_SIZE, (uint32_t)(totsect - FRONT_PORCH));
	put16(lab, o + P_BSIZE, (uint16_t)m->bsize);
	put16(lab, o + P_FSIZE, (uint16_t)m->fsize);
	lab[o + P_OPT] = 't';
	put16(lab, o + P_CPG, (uint16_t)m->cpg);
	put16(lab, o + P_DENSITY, DENSITY);
	lab[o + P_MINFREE] = MINFREE;
	lab[o + P_NEWFS] = 1;
	snprintf((char *)lab + o + P_MOUNTPT, 16, "/");
	lab[o + P_AUTOMNT] = 0;
	snprintf((char *)lab + o + P_TYPE, 8, "4.3BSD");

	put16(lab, DL_V3_CHECKSUM, nufs_label_checksum(lab, DL_V3_CHECKSUM));
}

/* --- the superblock ------------------------------------------------------ */

static int
ilog2(int x)
{
	int n = 0;

	while ((1 << n) < x)
		n++;
	return n;
}

static void
build_super(const struct mkfs *m, uint8_t *sb)
{
	int cylno, rpos, blk, nblk, i;
	int16_t post[NUFS_MAXCPG][NUFS_NRPOS];

	memset(sb, 0, NUFS_SBSIZE);
	put32(sb, FS_SBLKNO, (uint32_t)m->sblkno);
	put32(sb, FS_CBLKNO, (uint32_t)m->cblkno);
	put32(sb, FS_IBLKNO, (uint32_t)m->iblkno);
	put32(sb, FS_DBLKNO, (uint32_t)m->dblkno);
	put32(sb, FS_CGOFFSET, (uint32_t)m->cgoffset);
	put32(sb, FS_CGMASK, (uint32_t)m->cgmask);
	put32(sb, FS_TIME, (uint32_t)time(NULL));
	put32(sb, FS_SIZE, (uint32_t)m->fssize);
	/*
	 * Data fragments: everything except each group's metadata. Group 0
	 * also gives up its boot area and the cylinder group summary.
	 */
	put32(sb, FS_DSIZE, (uint32_t)(m->fssize -
	    (m->dblkno + HOWMANY(m->cssize, m->fsize)) -
	    (m->ncg - 1) * (m->dblkno - m->sblkno)));
	put32(sb, FS_NCG, (uint32_t)m->ncg);
	put32(sb, FS_BSIZE, (uint32_t)m->bsize);
	put32(sb, FS_FSIZE, (uint32_t)m->fsize);
	put32(sb, FS_FRAG, (uint32_t)m->frag);
	put32(sb, FS_MINFREE, MINFREE);
	put32(sb, FS_ROTDELAY, 4);
	put32(sb, FS_RPS, RPM / 60);
	put32(sb, FS_BMASK, (uint32_t)~(m->bsize - 1));
	put32(sb, FS_FMASK, (uint32_t)~(m->fsize - 1));
	put32(sb, FS_BSHIFT, (uint32_t)ilog2(m->bsize));
	put32(sb, FS_FSHIFT, (uint32_t)ilog2(m->fsize));
	put32(sb, FS_MAXCONTIG, 1);
	put32(sb, FS_MAXBPG, (uint32_t)(m->bsize / 4 / 8));
	put32(sb, FS_FRAGSHIFT, (uint32_t)ilog2(m->frag));
	put32(sb, FS_FSBTODB, 0);
	put32(sb, FS_SBSIZE, 2048);
	put32(sb, FS_CSMASK, (uint32_t)~(m->bsize / 16 - 1));
	put32(sb, FS_CSSHIFT, (uint32_t)ilog2(m->bsize / 16));
	put32(sb, FS_NINDIR, (uint32_t)m->nindir);
	put32(sb, FS_INOPB, (uint32_t)m->inopb);
	put32(sb, FS_NSPF, 1);
	put32(sb, FS_OPTIM, NUFS_OPTTIME);
	put32(sb, FS_CSADDR, (uint32_t)m->csaddr);
	put32(sb, FS_CSSIZE, (uint32_t)m->cssize);
	put32(sb, FS_CGSIZE, (uint32_t)m->cgsize);
	put32(sb, FS_NTRAK, (uint32_t)m->ntrak);
	put32(sb, FS_NSECT, (uint32_t)m->nsect);
	put32(sb, FS_SPC, (uint32_t)m->spc);
	put32(sb, FS_NCYL, (uint32_t)m->ncyl);
	put32(sb, FS_CPG, (uint32_t)m->cpg);
	put32(sb, FS_IPG, (uint32_t)m->ipg);
	put32(sb, FS_FPG, (uint32_t)m->fpg);
	sb[FS_FMOD] = 0;
	sb[FS_STATE] = NUFS_STATE_CLEAN;
	sb[FS_RONLY] = 0;
	snprintf((char *)sb + FS_FSMNT, NUFS_MAXMNTLEN, "/");
	put32(sb, FS_CGROTOR, 0);
	put32(sb, FS_CPC, (uint32_t)m->cpc);
	put32(sb, FS_MAGIC, NUFS_FS_MAGIC);

	/*
	 * Rotational layout: fs_postbl[cyl][rpos] is the first block at that
	 * position, and fs_rotbl chains the rest as deltas.
	 */
	for (cylno = 0; cylno < NUFS_MAXCPG; cylno++)
		for (rpos = 0; rpos < NUFS_NRPOS; rpos++)
			post[cylno][rpos] = -1;
	nblk = m->cpc * m->spc / m->frag;
	for (blk = 0; blk < nblk; blk++) {
		int b = blk * m->frag;
		int cyl = cbtocylno(m, b) % m->cpc;
		int rp = cbtorpos(m, b);
		int prev;

		if (post[cyl][rp] == -1) {
			post[cyl][rp] = (int16_t)blk;
			continue;
		}
		prev = post[cyl][rp];
		while (sb[FS_ROTBL + prev] != 0)
			prev += sb[FS_ROTBL + prev];
		sb[FS_ROTBL + prev] = (uint8_t)(blk - prev);
	}
	for (cylno = 0; cylno < NUFS_MAXCPG; cylno++)
		for (rpos = 0; rpos < NUFS_NRPOS; rpos++)
			put16(sb, FS_POSTBL + 2 * (cylno * NUFS_NRPOS + rpos),
			    (uint16_t)post[cylno][rpos]);
	i = FS_ROTBL + nblk;
	if (i > 2048) {
		fprintf(stderr, "nextufs: rotational table does not fit\n");
		exit(1);
	}
}

/* --- cylinder groups ----------------------------------------------------- */

static void
build_cg(const struct mkfs *m, int c, uint8_t *cg, int32_t *cs)
{
	int ndblk, dlower, dupper, i, base;
	int nbfree = 0, nffree = 0, nifree, run;
	uint8_t *map;

	memset(cg, 0, (size_t)m->cgsize);
	base = m->fpg * c;
	ndblk = m->fssize - base;
	if (ndblk > m->fpg)
		ndblk = m->fpg;
	dlower = cgstart_of(m, c) - base + m->sblkno;
	dupper = cgstart_of(m, c) - base + m->dblkno;
	if (c == 0)
		dupper += HOWMANY(m->cssize, m->fsize);

	put32(cg, CG_TIME, (uint32_t)time(NULL));
	put32(cg, CG_CGX, (uint32_t)c);
	put16(cg, CG_NCYL, (uint16_t)(c == m->ncg - 1 ? m->ncyl % m->cpg :
	    m->cpg));
	put16(cg, CG_NIBLK, (uint16_t)m->ipg);
	put32(cg, CG_NDBLK, (uint32_t)ndblk);
	put32(cg, CG_MAGIC, NUFS_CG_MAGIC);

	map = cg + CG_FREE;
	for (i = 0; i < ndblk; i++) {
		int used = (i >= dlower && i < dupper) || (c == 0 && i < dlower);

		if (!used)
			map[i >> 3] |= (uint8_t)(1 << (i & 7));
	}

	/* Count what the map says, exactly as the allocator will. */
	for (i = 0; i + m->frag <= ndblk; i += m->frag) {
		int k, allfree = 1;

		for (k = 0; k < m->frag; k++)
			if (!((map[(i + k) >> 3] >> ((i + k) & 7)) & 1))
				allfree = 0;
		if (allfree) {
			int cyl = cbtocylno(m, i), rp = cbtorpos(m, i);

			nbfree++;
			if (cyl < NUFS_MAXCPG) {
				put32(cg, CG_BTOT + 4 * cyl, (uint32_t)
				    ((int32_t)((cg[CG_BTOT + 4 * cyl] << 24) |
				    (cg[CG_BTOT + 4 * cyl + 1] << 16) |
				    (cg[CG_BTOT + 4 * cyl + 2] << 8) |
				    cg[CG_BTOT + 4 * cyl + 3]) + 1));
				put16(cg, CG_B + 2 * (cyl * NUFS_NRPOS + rp),
				    (uint16_t)(((cg[CG_B + 2 * (cyl *
				    NUFS_NRPOS + rp)] << 8) | cg[CG_B + 2 *
				    (cyl * NUFS_NRPOS + rp) + 1]) + 1));
			}
			continue;
		}
		run = 0;
		for (k = 0; k < m->frag; k++) {
			if ((map[(i + k) >> 3] >> ((i + k) & 7)) & 1) {
				run++;
				nffree++;
			} else {
				if (run > 0 && run < m->frag)
					put32(cg, CG_FRSUM + 4 * run,
					    (uint32_t)(((cg[CG_FRSUM + 4 * run]
					    << 24) | (cg[CG_FRSUM + 4 * run + 1]
					    << 16) | (cg[CG_FRSUM + 4 * run + 2]
					    << 8) | cg[CG_FRSUM + 4 * run + 3])
					    + 1));
				run = 0;
			}
		}
		if (run > 0 && run < m->frag)
			put32(cg, CG_FRSUM + 4 * run,
			    (uint32_t)(((cg[CG_FRSUM + 4 * run] << 24) |
			    (cg[CG_FRSUM + 4 * run + 1] << 16) |
			    (cg[CG_FRSUM + 4 * run + 2] << 8) |
			    cg[CG_FRSUM + 4 * run + 3]) + 1));
	}

	nifree = m->ipg;
	if (c == 0) {
		/* Inodes 0 and 1 are reserved and never free. */
		cg[CG_IUSED] |= 0x03;
		nifree -= NUFS_ROOTINO;
	}
	put32(cg, CG_CS + 0, 0);
	put32(cg, CG_CS + 4, (uint32_t)nbfree);
	put32(cg, CG_CS + 8, (uint32_t)nifree);
	put32(cg, CG_CS + 12, (uint32_t)nffree);
	cs[0] = 0;
	cs[1] = nbfree;
	cs[2] = nifree;
	cs[3] = nffree;
}

/* --- driver -------------------------------------------------------------- */

int
nufs_mkfs(const char *path, long long totsect, const char *name, int quiet)
{
	struct mkfs m;
	struct nufs *v;
	struct nufs_dinode root;
	uint8_t *lab, *sb, *cgbuf, *zero, *csum;
	char err[256];
	FILE *f;
	int c, i, rc = -1;
	int32_t tot[4] = { 0, 0, 0, 0 };
	long long need;

	memset(&m, 0, sizeof(m));
	m.secsize = SECSIZE;
	m.bsize = BSIZE;
	m.fsize = FSIZE;
	m.frag = FRAGS;
	m.ntrak = NTRAK;
	m.nsect = NSECT;
	m.spc = NTRAK * NSECT;
	m.cpg = CPG;
	m.inopb = m.bsize / NUFS_DINODE_SIZE;
	m.nindir = m.bsize / 4;
	m.cgoffset = 16;
	m.cgmask = (int)~(unsigned)(m.ntrak - 1);
	m.cpc = 1;
	m.fpg = m.cpg * m.spc;
	m.partoff = (long long)FRONT_PORCH * m.secsize;
	m.fssize = (int)(totsect - FRONT_PORCH);
	if (m.fssize < 4 * m.fpg) {
		fprintf(stderr, "nextufs: %lld sectors is too small\n", totsect);
		return -1;
	}
	m.ipg = ROUNDUP((long long)m.fpg * m.fsize / DENSITY, m.inopb);
	if (m.ipg > NUFS_MAXIPG)
		m.ipg = NUFS_MAXIPG;
	m.sblkno = 16;
	m.cblkno = 24;
	m.iblkno = 32;
	m.dblkno = m.iblkno + (m.ipg / m.inopb) * m.frag;
	/*
	 * Size to whole cylinders and allow a short last cylinder group, the
	 * way newfs does. fsck rebuilds the last group's cg_ncyl as
	 * fs_ncyl % fs_cpg, so a size that divides evenly would make it
	 * expect zero there; keeping the remainder is what real volumes have.
	 */
	m.ncyl = m.fssize / m.spc;
	m.fssize = m.ncyl * m.spc;
	m.ncg = HOWMANY(m.ncyl, m.cpg);
	if (m.ncg >= 2 && m.fssize - m.fpg * (m.ncg - 1) < m.dblkno + 2 * m.frag) {
		m.ncg--;			/* the tail group cannot hold its own metadata */
		m.ncyl = m.ncg * m.cpg;
		m.fssize = m.ncyl * m.spc;
	}
	if (m.ncg < 1) {
		fprintf(stderr, "nextufs: no room for a cylinder group\n");
		return -1;
	}
	m.cssize = (int)ROUNDUP((long long)m.ncg * 16, m.fsize);
	m.csaddr = m.dblkno;
	m.cgsize = (int)ROUNDUP(CG_FREE + HOWMANY(m.fpg, 8), m.fsize);
	if (m.cgsize > m.bsize) {
		fprintf(stderr, "nextufs: cylinder group map does not fit\n");
		return -1;
	}
	if (m.cssize > NUFS_MAXCSBUFS * m.bsize) {
		fprintf(stderr, "nextufs: too many cylinder groups\n");
		return -1;
	}

	need = m.partoff + (long long)m.fssize * m.fsize;
	f = fopen(path, "r+b");
	if (f == NULL)
		f = fopen(path, "w+b");
	if (f == NULL) {
		fprintf(stderr, "nextufs: cannot open %s\n", path);
		return -1;
	}
	if (fseeko(f, (off_t)(need - 1), SEEK_SET) != 0 || fputc(0, f) == EOF) {
		fprintf(stderr, "nextufs: cannot size %s to %lld bytes\n", path,
		    need);
		fclose(f);
		return -1;
	}

	lab = calloc(1, NUFS_SBSIZE);
	sb = calloc(1, NUFS_SBSIZE);
	cgbuf = calloc(1, (size_t)m.cgsize);
	zero = calloc(1, (size_t)m.bsize);
	csum = calloc(1, (size_t)m.cssize);
	if (lab == NULL || sb == NULL || cgbuf == NULL || zero == NULL ||
	    csum == NULL) {
		fprintf(stderr, "nextufs: out of memory\n");
		fclose(f);
		return -1;
	}

	build_label(&m, lab, name, totsect);
	for (i = 0; i < NLABELS; i++) {
		fseeko(f, (off_t)(i * LABEL_STRIDE), SEEK_SET);
		fwrite(lab, 1, NUFS_SBSIZE, f);
	}
	build_super(&m, sb);

	for (c = 0; c < m.ncg; c++) {
		int32_t cs[4];
		int nb;

		build_cg(&m, c, cgbuf, cs);
		for (i = 0; i < 4; i++) {
			put32((uint8_t *)csum, c * 16 + 4 * i, (uint32_t)cs[i]);
			tot[i] += cs[i];
		}
		fseeko(f, (off_t)(m.partoff + (long long)(cgstart_of(&m, c) +
		    m.cblkno) * m.fsize), SEEK_SET);
		fwrite(cgbuf, 1, (size_t)m.cgsize, f);

		/* per-group superblock backup */
		fseeko(f, (off_t)(m.partoff + (long long)(cgstart_of(&m, c) +
		    m.sblkno) * m.fsize), SEEK_SET);
		fwrite(sb, 1, 2048, f);

		/* zero this group's inode blocks */
		fseeko(f, (off_t)(m.partoff + (long long)(cgstart_of(&m, c) +
		    m.iblkno) * m.fsize), SEEK_SET);
		for (nb = 0; nb < m.ipg / m.inopb; nb++)
			fwrite(zero, 1, (size_t)m.bsize, f);
	}
	for (i = 0; i < 4; i++)
		put32(sb, FS_CSTOTAL + 4 * i, (uint32_t)tot[i]);

	fseeko(f, (off_t)(m.partoff + (long long)m.csaddr * m.fsize), SEEK_SET);
	fwrite(csum, 1, (size_t)m.cssize, f);
	fseeko(f, (off_t)(m.partoff + NUFS_SBOFF), SEEK_SET);
	fwrite(sb, 1, 2048, f);
	if (fclose(f) != 0) {
		fprintf(stderr, "nextufs: writing %s failed\n", path);
		goto out;
	}

	/* Build the root and lost+found with the ordinary allocator. */
	v = nufs_open(path, "a", 1, err, sizeof(err));
	if (v == NULL) {
		fprintf(stderr, "nextufs: %s: %s\n", path, err);
		goto out;
	}
	{
		uint8_t blk[NUFS_DIRBLKSIZ];
		uint32_t ino = nufs_alloc_inode(v, 0, 1);
		int dot = NUFS_DIRSIZ(1);
		uint32_t now = (uint32_t)time(NULL);

		if (ino != NUFS_ROOTINO) {
			fprintf(stderr, "nextufs: root came out as inode %u\n",
			    ino);
			nufs_close(v);
			goto out;
		}
		memset(&root, 0, sizeof(root));
		root.ino = ino;
		root.mode = 0040755;
		root.nlink = 2;
		root.atime = root.mtime = root.ctime = now;
		root.gen = now;
		if (nufs_inode_write(v, &root) != 0)
			goto closefail;

		memset(blk, 0, sizeof(blk));
		nufs_put32(v, blk, DIR_INO, ino);
		nufs_put16(v, blk, DIR_RECLEN, (uint16_t)dot);
		nufs_put16(v, blk, DIR_NAMLEN, 1);
		blk[DIR_NAME] = '.';
		nufs_put32(v, blk, dot + DIR_INO, ino);
		nufs_put16(v, blk, dot + DIR_RECLEN,
		    (uint16_t)(NUFS_DIRBLKSIZ - dot));
		nufs_put16(v, blk, dot + DIR_NAMLEN, 2);
		blk[dot + DIR_NAME] = '.';
		blk[dot + DIR_NAME + 1] = '.';
		if (nufs_file_write(v, &root, blk, 0, NUFS_DIRBLKSIZ) !=
		    NUFS_DIRBLKSIZ)
			goto closefail;

		if (nufs_mkdir(v, "/lost+found", 0700, 0, 0) != 0)
			goto closefail;
		/* Give lost+found the usual eight empty directory blocks. */
		{
			struct nufs_dinode lf;

			if (nufs_lookup(v, "/lost+found", &lf) != 0)
				goto closefail;
			memset(blk, 0, sizeof(blk));
			nufs_put16(v, blk, DIR_RECLEN, NUFS_DIRBLKSIZ);
			for (i = 1; i < 8; i++)
				if (nufs_file_write(v, &lf, blk,
				    (long long)i * NUFS_DIRBLKSIZ,
				    NUFS_DIRBLKSIZ) != NUFS_DIRBLKSIZ)
					goto closefail;
		}
	}
	v->sb[FS_FMOD] = 0;			/* a fresh volume is not dirty */
	v->sb[FS_STATE] = NUFS_STATE_CLEAN;
	v->dirty_sb = 1;
	if (nufs_flush(v) != 0)
		goto closefail;
	if (!quiet)
		printf("%s: %d fragments of %d bytes in %d cylinder groups,"
		    " %d inodes each\n", path, m.fssize, m.fsize, m.ncg, m.ipg);
	nufs_close(v);
	rc = 0;
	goto out;

closefail:
	fprintf(stderr, "nextufs: %s\n", v->err);
	nufs_close(v);
out:
	free(lab);
	free(sb);
	free(cgbuf);
	free(zero);
	free(csum);
	return rc;
}
