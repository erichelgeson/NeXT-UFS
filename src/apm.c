/*
 * The Apple Partition Map, which is how an A/UX disk is carved up.
 *
 * A/UX has no NeXT disk label. Block 0 is Apple's driver descriptor ("ER")
 * and block 1 onwards is an array of one-block partition entries ("PM"),
 * each of which knows how many entries the whole map has. A UNIX slice is
 * typed Apple_UNIX_SVR2; a disk usually carries three of them (root, swap
 * and the small "Eschatology" crash-recovery area), so which one holds the
 * filesystem is decided in fs.c, not here.
 */
#include "nextufs.h"
#include <string.h>

#define APM_BLOCK0_SIG	0x4552			/* "ER" */
#define APM_ENTRY_SIG	0x504d			/* "PM" */

/* Block 0 */
#define DDM_SIG		0			/* short */
#define DDM_BLKSIZE	2			/* short */
#define DDM_BLKCOUNT	4

/* One partition map entry */
#define PM_SIG		0			/* short */
#define PM_MAPBLKCNT	4
#define PM_PYPARTSTART	8
#define PM_PARTBLKCNT	12
#define PM_PARTNAME	16			/* char[32] */
#define PM_PARTYPE	48			/* char[32] */

static uint32_t
be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t
be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Partition names are Pascal-ish fixed fields; trim to something printable. */
static void
getstr(char *dst, const uint8_t *src, int n)
{
	int i;

	for (i = 0; i < n && src[i] != '\0'; i++)
		dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : '?';
	dst[i] = '\0';
	while (i > 0 && dst[i - 1] == ' ')
		dst[--i] = '\0';
}

int
nufs_apm_read(struct nufs *v, struct nufs_apm *m)
{
	uint8_t blk[NUFS_APM_BLKMAX];
	int bs = 512, i, n;

	memset(m, 0, sizeof(*m));
	if (nufs_pread(v, blk, 0, sizeof(blk)) != 0)
		return -1;
	if (be16(blk + DDM_SIG) == APM_BLOCK0_SIG) {
		int got = be16(blk + DDM_BLKSIZE);

		if (got == 512 || got == 1024 || got == 2048)
			bs = got;
	}
	/*
	 * The driver descriptor is advisory: some images carry a map with no
	 * usable block 0, so what settles it is a "PM" entry at block 1.
	 */
	if (nufs_pread(v, blk, bs, (size_t)bs) != 0 ||
	    be16(blk + PM_SIG) != APM_ENTRY_SIG)
		return -1;

	m->blocksize = bs;
	n = (int)be32(blk + PM_MAPBLKCNT);
	if (n <= 0 || n > NUFS_MAXAPM)
		n = NUFS_MAXAPM;
	for (i = 0; i < n; i++) {
		struct nufs_apm_part *p = &m->part[m->n];

		if (i > 0 && (nufs_pread(v, blk, (long long)bs * (i + 1),
		    (size_t)bs) != 0 || be16(blk + PM_SIG) != APM_ENTRY_SIG))
			break;
		p->start = be32(blk + PM_PYPARTSTART);
		p->size = be32(blk + PM_PARTBLKCNT);
		getstr(p->name, blk + PM_PARTNAME, 32);
		getstr(p->type, blk + PM_PARTYPE, 32);
		m->n++;
	}
	m->valid = m->n > 0;
	return m->valid ? 0 : -1;
}

int
nufs_apm_is_unix(const struct nufs_apm_part *p)
{
	return strcmp(p->type, "Apple_UNIX_SVR2") == 0;
}

void
nufs_apm_print(const struct nufs_apm *m, FILE *out)
{
	int i;

	fprintf(out, "apm       %d partitions, %d-byte blocks\n", m->n,
	    m->blocksize);
	for (i = 0; i < m->n; i++) {
		const struct nufs_apm_part *p = &m->part[i];

		fprintf(out, "  %d: start %9u  size %9u  %-20s %s\n", i + 1,
		    p->start, p->size, p->type, p->name);
	}
}
