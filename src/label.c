#include "nextufs.h"
#include <string.h>

/*
 * NeXT's ones-complement label checksum: a big-endian 16-bit word sum over
 * everything up to the checksum field, with the carries folded back in.
 * Matches NetBSD's next68k nextstep_checksum().
 */
uint16_t
nufs_label_checksum(const uint8_t *buf, int limit)
{
	unsigned int sum = 0;
	int i;

	for (i = 0; i + 1 < limit; i += 2)
		sum += ((unsigned)buf[i] << 8) + buf[i + 1];
	sum = (sum & 0xffff) + (sum >> 16);
	sum += sum >> 16;
	return (uint16_t)(sum & 0xffff);
}

static void
getstr(char *dst, const uint8_t *src, int n)
{
	memcpy(dst, src, n);
	dst[n] = '\0';
}

int
nufs_label_read(struct nufs *v, struct nufs_label *l)
{
	const uint8_t *r = l->raw;
	int i, v3, cksumoff;
	uint16_t want, got;

	memset(l, 0, sizeof(*l));
	if (nufs_pread(v, l->raw, 0, sizeof(l->raw)) != 0)
		return -1;
	r = l->raw;
	getstr(l->version, r + DL_VERSION, 4);
	if (strcmp(l->version, "dlV3") && strcmp(l->version, "dlV2") &&
	    strcmp(l->version, "NeXT"))
		return -1;
	v3 = strcmp(l->version, "dlV3") == 0;

	/* The label is always big-endian; it is written by m68k firmware. */
	cksumoff = v3 ? DL_V3_CHECKSUM : DL_CHECKSUM;
	want = (uint16_t)((r[cksumoff] << 8) | r[cksumoff + 1]);
	got = nufs_label_checksum(r, cksumoff);
	l->valid = (want == got);

#define G32(o) ((int32_t)(((uint32_t)r[o] << 24) | ((uint32_t)r[(o)+1] << 16) | \
	    ((uint32_t)r[(o)+2] << 8) | r[(o)+3]))
#define G16(o) ((int16_t)(((uint16_t)r[o] << 8) | r[(o)+1]))
	l->size = G32(DL_SIZE);
	getstr(l->name, r + DL_LABEL, 24);
	getstr(l->drive, r + DT_NAME, 24);
	getstr(l->drvtype, r + DT_TYPE, 24);
	l->secsize = G32(DT_SECSIZE);
	l->ntracks = G32(DT_NTRACKS);
	l->nsectors = G32(DT_NSECTORS);
	l->ncylinders = G32(DT_NCYLINDERS);
	l->rpm = G32(DT_RPM);
	l->front = G16(DT_FRONT);
	l->back = G16(DT_BACK);
	l->rootpart = (char)r[DT_ROOTPART];
	l->rwpart = (char)r[DT_RWPART];

	for (i = 0; i < NUFS_NPART; i++) {
		int o = DT_PARTITIONS + i * NUFS_PARTSIZE;
		struct nufs_part *p = &l->part[i];

		p->base = G32(o + P_BASE);
		p->size = G32(o + P_SIZE);
		p->bsize = G16(o + P_BSIZE);
		p->fsize = G16(o + P_FSIZE);
		p->opt = (char)r[o + P_OPT];
		p->cpg = G16(o + P_CPG);
		p->density = G16(o + P_DENSITY);
		p->minfree = (int8_t)r[o + P_MINFREE];
		p->newfs = (char)r[o + P_NEWFS];
		getstr(p->mountpt, r + o + P_MOUNTPT, 16);
		p->automnt = (char)r[o + P_AUTOMNT];
		getstr(p->type, r + o + P_TYPE, 8);
	}
#undef G32
#undef G16
	return 0;
}

void
nufs_label_print(const struct nufs_label *l, FILE *out)
{
	int i;

	fprintf(out, "label     %s  \"%s\"  checksum %s\n", l->version, l->name,
	    l->valid ? "ok" : "BAD");
	fprintf(out, "drive     %s (%s)\n", l->drive, l->drvtype);
	fprintf(out, "geometry  %d sectors of %d bytes, %d tracks, %d sect/track,"
	    " %d cyl, %d rpm\n", l->size, l->secsize, l->ntracks, l->nsectors,
	    l->ncylinders, l->rpm);
	fprintf(out, "porch     front %d, back %d   root '%c' rw '%c'\n",
	    l->front, l->back, l->rootpart ? l->rootpart : '-',
	    l->rwpart ? l->rwpart : '-');
	for (i = 0; i < NUFS_NPART; i++) {
		const struct nufs_part *p = &l->part[i];

		if (p->base < 0 || p->size <= 0)
			continue;
		fprintf(out, "  %c: base %8d  size %9d  bsize %5d  fsize %5d"
		    "  cpg %3d  %-8s %s\n", 'a' + i, p->base, p->size,
		    p->bsize, p->fsize, p->cpg, p->type, p->mountpt);
	}
}
