/*
 * Consistency check and repair.
 *
 * The passes mirror 4.3BSD fsck, in the order that makes each one's results
 * usable by the next:
 *
 *	1  every used inode's block map      -> which fragments are claimed
 *	2  the directory tree from the root  -> reference counts, "." and ".."
 *	3  link counts and unreferenced inodes
 *	4  the free maps and every summary counter, rebuilt and compared
 */
#include "nextufs.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

struct fsck {
	struct nufs	*v;
	int		fix;
	int		problems;
	int		fixed;

	uint8_t		*claimed;	/* one bit per fragment */
	uint8_t		*dup;		/* fragments claimed more than once */
	uint32_t	ninodes;
	uint8_t		*used;		/* inode is marked used in cg_iused */
	uint8_t		*isdir;
	uint16_t	*ondisk_nlink;
	uint16_t	*refs;		/* references found in directories */
	uint32_t	*parent;
};

static void
problem(struct fsck *f, const char *fmt, ...)
{
	va_list ap;

	f->problems++;
	fputs("  ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

static int
bit_get(const uint8_t *m, uint32_t i)
{
	return (m[i >> 3] >> (i & 7)) & 1;
}

static void
bit_set(uint8_t *m, uint32_t i)
{
	m[i >> 3] |= (uint8_t)(1 << (i & 7));
}

/* Fragments a cylinder group holds for its own metadata, cgbase-relative. */
static void
meta_range(const struct nufs *v, int cg, int *lo, int *hi)
{
	int off = nufs_cgstart(v, cg) - nufs_cgbase(v, cg);

	if (cg == 0) {
		*lo = 0;		/* the boot blocks live here too */
		*hi = v->dblkno + (v->cssize + v->fsize - 1) / v->fsize;
	} else {
		*lo = off + v->sblkno;
		*hi = off + v->dblkno;
	}
}

static void
claim(struct fsck *f, int frag, int n, const char *what, uint32_t ino)
{
	int i;

	for (i = 0; i < n; i++) {
		uint32_t b = (uint32_t)(frag + i);

		if (frag < 0 || b >= (uint32_t)f->v->size) {
			problem(f, "%s of inode %u points outside the volume"
			    " (fragment %d)", what, ino, frag + i);
			return;
		}
		if (bit_get(f->claimed, b)) {
			if (!bit_get(f->dup, b))
				problem(f, "fragment %u is claimed twice"
				    " (again by %s of inode %u)", b, what, ino);
			bit_set(f->dup, b);
		}
		bit_set(f->claimed, b);
	}
}

/* Walk one inode's block map, claiming data and indirect fragments. */
static int
walk_inode(struct fsck *f, const struct nufs_dinode *dp, int *nfrags)
{
	struct nufs *v = f->v;
	int lastlbn, lbn, i;

	*nfrags = 0;

	if ((dp->flags & NUFS_IC_FASTLINK) != 0)
		return 0;
	lastlbn = (int)((dp->size + v->bsize - 1) / v->bsize);
	for (lbn = 0; lbn < lastlbn; lbn++) {
		int frag;

		if (nufs_bmap(v, dp, lbn, &frag) != 0)
			return -1;
		if (frag != 0) {
			int n = nufs_blk_frags(v, dp, lbn);

			claim(f, frag, n, "a data block", dp->ino);
			*nfrags += n;
		}
	}
	/* Claim the indirect blocks themselves. */
	for (i = 0; i < NUFS_NIADDR; i++) {
		int stack[3], depth = 0, blk = dp->ib[i];
		uint8_t *buf;

		if (blk == 0)
			continue;
		claim(f, blk, v->frag, "an indirect block", dp->ino);
		*nfrags += v->frag;
		if (i == 0)
			continue;
		buf = malloc((size_t)v->bsize);
		if (buf == NULL)
			return -1;
		stack[depth++] = blk;
		/* Only two extra levels exist; recurse iteratively. */
		while (depth > 0) {
			int cur = stack[--depth];
			int lvl = (cur == dp->ib[i]) ? i : i - 1;
			int j;

			if (nufs_read_frags(v, cur, v->frag, buf) != 0) {
				free(buf);
				return -1;
			}
			for (j = 0; j < v->nindir; j++) {
				int child = (int32_t)nufs_get32(v, buf, 4 * j);

				if (child == 0 || lvl <= 0)
					continue;
				claim(f, child, v->frag, "an indirect block",
				    dp->ino);
				*nfrags += v->frag;
				if (lvl > 1 && depth < 3)
					stack[depth++] = child;
			}
		}
		free(buf);
	}
	return 0;
}

struct dirwalk {
	struct fsck	*f;
	uint32_t	ino;
	int		sawdot, sawdotdot;
	uint32_t	dotdot;
};

static int walk_dir(struct fsck *f, uint32_t ino, uint32_t parent);

static int
dir_cb(void *arg, uint32_t ino, const char *name, int namlen)
{
	struct dirwalk *w = arg;
	struct fsck *f = w->f;

	(void)namlen;
	if (ino >= f->ninodes) {
		problem(f, "directory inode %u names \"%s\" with out-of-range"
		    " inode %u", w->ino, name, ino);
		return 0;
	}
	if (strcmp(name, ".") == 0) {
		w->sawdot = 1;
		if (ino != w->ino)
			problem(f, "inode %u has \".\" pointing at %u",
			    w->ino, ino);
		f->refs[ino]++;
		return 0;
	}
	if (strcmp(name, "..") == 0) {
		w->sawdotdot = 1;
		w->dotdot = ino;
		f->refs[ino]++;
		return 0;
	}
	f->refs[ino]++;
	if (f->isdir[ino]) {
		if (f->parent[ino] != 0)
			problem(f, "directory inode %u is linked from two"
			    " places", ino);
		else
			return walk_dir(f, ino, w->ino);
	}
	return 0;
}

static int
walk_dir(struct fsck *f, uint32_t ino, uint32_t parent)
{
	struct nufs_dinode d;
	struct dirwalk w;

	memset(&w, 0, sizeof(w));
	w.f = f;
	w.ino = ino;
	f->parent[ino] = parent;
	if (nufs_inode_read(f->v, ino, &d) != 0)
		return -1;
	if (d.size % NUFS_DIRBLKSIZ != 0)
		problem(f, "directory inode %u has size %llu, not a multiple"
		    " of %d", ino, (unsigned long long)d.size, NUFS_DIRBLKSIZ);
	if (nufs_readdir(f->v, &d, dir_cb, &w) != 0) {
		problem(f, "directory inode %u could not be read: %s", ino,
		    f->v->err);
		return 0;
	}
	if (!w.sawdot)
		problem(f, "directory inode %u has no \".\"", ino);
	if (!w.sawdotdot)
		problem(f, "directory inode %u has no \"..\"", ino);
	else if (w.dotdot != parent)
		problem(f, "directory inode %u has \"..\" pointing at %u,"
		    " expected %u", ino, w.dotdot, parent);
	return 0;
}

int
nufs_fsck(struct nufs *v, int fix)
{
	struct fsck f;
	uint32_t nfrag = (uint32_t)v->size;
	uint32_t ino;
	int cg, rc = 0;
	int32_t tot_ndir = 0, tot_nbfree = 0, tot_nifree = 0, tot_nffree = 0;

	memset(&f, 0, sizeof(f));
	f.v = v;
	f.fix = fix;
	f.ninodes = (uint32_t)(v->ipg * v->ncg);
	f.claimed = calloc((nfrag + 7) / 8, 1);
	f.dup = calloc((nfrag + 7) / 8, 1);
	f.used = calloc(f.ninodes, 1);
	f.isdir = calloc(f.ninodes, 1);
	f.ondisk_nlink = calloc(f.ninodes, sizeof(*f.ondisk_nlink));
	f.refs = calloc(f.ninodes, sizeof(*f.refs));
	f.parent = calloc(f.ninodes, sizeof(*f.parent));
	if (f.claimed == NULL || f.dup == NULL || f.used == NULL ||
	    f.isdir == NULL || f.ondisk_nlink == NULL || f.refs == NULL ||
	    f.parent == NULL) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}

	/* Pass 1: inodes and their block maps. */
	printf("** pass 1: inodes and block maps\n");
	for (cg = 0; cg < v->ncg; cg++) {
		int lo, hi, i;

		if (nufs_cg_load(v, cg) != 0) {
			problem(&f, "%s", v->err);
			continue;
		}
		meta_range(v, cg, &lo, &hi);
		for (i = lo; i < hi; i++)
			bit_set(f.claimed, (uint32_t)(nufs_cgbase(v, cg) + i));

		for (i = 0; i < v->ipg; i++) {
			struct nufs_dinode d;

			if (!((v->cgbuf[CG_IUSED + (i >> 3)] >> (i & 7)) & 1))
				continue;
			ino = (uint32_t)(cg * v->ipg + i);
			if (ino < NUFS_ROOTINO)
				continue;
			f.used[ino] = 1;
			if (nufs_inode_read(v, ino, &d) != 0) {
				problem(&f, "inode %u could not be read", ino);
				continue;
			}
			if (d.mode == 0) {
				problem(&f, "inode %u is marked used but has"
				    " mode 0", ino);
				continue;
			}
			f.ondisk_nlink[ino] = (uint16_t)d.nlink;
			f.isdir[ino] = (d.mode & 0170000) == 0040000;
			{
				int nfrags = 0;

				if (walk_inode(&f, &d, &nfrags) != 0)
					problem(&f, "inode %u: %s", ino, v->err);
				else if ((uint32_t)(nfrags << v->fsbtodb) !=
				    d.blocks)
					problem(&f, "inode %u holds %d"
					    " fragments but di_blocks says %u",
					    ino, nfrags << v->fsbtodb, d.blocks);
			}
		}
	}

	/* Pass 2: the directory tree. */
	printf("** pass 2: directory tree\n");
	if (walk_dir(&f, NUFS_ROOTINO, NUFS_ROOTINO) != 0)
		problem(&f, "the root directory could not be walked");

	/* Pass 3: link counts. */
	printf("** pass 3: link counts\n");
	for (ino = NUFS_ROOTINO; ino < f.ninodes; ino++) {
		if (!f.used[ino]) {
			if (f.refs[ino] != 0)
				problem(&f, "inode %u is referenced %u times"
				    " but is marked free", ino, f.refs[ino]);
			continue;
		}
		if (f.refs[ino] == 0) {
			problem(&f, "inode %u is used but nothing refers"
			    " to it", ino);
			continue;
		}
		if (f.refs[ino] != f.ondisk_nlink[ino]) {
			problem(&f, "inode %u has link count %u, counted %u",
			    ino, f.ondisk_nlink[ino], f.refs[ino]);
			if (fix) {
				struct nufs_dinode d;

				if (nufs_inode_read(v, ino, &d) == 0) {
					d.nlink = (int16_t)f.refs[ino];
					if (nufs_inode_write(v, &d) == 0)
						f.fixed++;
				}
			}
		}
	}

	/* Pass 4: free maps and summary counters. */
	printf("** pass 4: free maps and summaries\n");
	for (cg = 0; cg < v->ncg; cg++) {
		int ndblk, base, i, nbfree = 0, nffree = 0, ndir = 0, nifree = 0;
		int badbits = 0;
		int frsum[NUFS_MAXFRAG];
		int btot[NUFS_MAXCPG];
		short bpos[NUFS_MAXCPG][NUFS_NRPOS];

		if (nufs_cg_load(v, cg) != 0)
			continue;
		memset(frsum, 0, sizeof(frsum));
		memset(btot, 0, sizeof(btot));
		memset(bpos, 0, sizeof(bpos));
		ndblk = (int32_t)nufs_get32(v, v->cgbuf, CG_NDBLK);
		base = nufs_cgbase(v, cg);

		for (i = 0; i < ndblk; i++) {
			uint32_t abs = (uint32_t)(base + i);
			int isfree = (v->cgbuf[CG_FREE + (i >> 3)] >> (i & 7)) & 1;
			int shouldfree = abs < nfrag && !bit_get(f.claimed, abs);

			if (isfree != shouldfree) {
				if (badbits++ == 0)
					problem(&f, "cylinder group %d: free map"
					    " disagrees with the inodes, first"
					    " at fragment %u (%s)", cg, abs,
					    isfree ? "free but in use" :
					    "in use but not free");
			}
		}

		/* Recount from the map as it should be. */
		for (i = 0; i + v->frag <= ndblk; i += v->frag) {
			int k, run = 0, allfree = 1;

			for (k = 0; k < v->frag; k++)
				if (bit_get(f.claimed, (uint32_t)(base + i + k)))
					allfree = 0;
			if (allfree) {
				int cyl = (i * v->nspf) / v->spc;
				int rp = (i * v->nspf) % v->spc % v->nsect *
				    NUFS_NRPOS / v->nsect;

				nbfree++;
				if (cyl < NUFS_MAXCPG) {
					btot[cyl]++;
					bpos[cyl][rp]++;
				}
				continue;
			}
			for (k = 0; k < v->frag; k++) {
				if (!bit_get(f.claimed,
				    (uint32_t)(base + i + k))) {
					run++;
					nffree++;
				} else {
					if (run > 0 && run < v->frag)
						frsum[run]++;
					run = 0;
				}
			}
			if (run > 0 && run < v->frag)
				frsum[run]++;
		}
		for (i = 0; i < v->ipg; i++) {
			uint32_t n = (uint32_t)(cg * v->ipg + i);

			if (n < f.ninodes && f.used[n]) {
				if (f.isdir[n])
					ndir++;
			} else if (n >= NUFS_ROOTINO || cg != 0)
				nifree++;
		}
		if (cg == 0)
			nifree -= 0;

#define CHECK32(what, off, want)						\
		do {							\
			int32_t got = (int32_t)nufs_get32(v, v->cgbuf, off); \
			if (got != (want)) {				\
				problem(&f, "cylinder group %d: %s is %d,"\
				    " counted %d", cg, what, got, (want)); \
				if (fix) {				\
					nufs_put32(v, v->cgbuf, off,	\
					    (uint32_t)(want));		\
					v->dirty_cg = 1;		\
					f.fixed++;			\
				}					\
			}						\
		} while (0)

		{
			int wantncyl = (cg == v->ncg - 1) ? v->ncyl % v->cpg :
			    v->cpg;
			int gotncyl = (int16_t)nufs_get16(v, v->cgbuf, CG_NCYL);
			int gotniblk = (int16_t)nufs_get16(v, v->cgbuf, CG_NIBLK);
			int gotcgx = (int32_t)nufs_get32(v, v->cgbuf, CG_CGX);
			int wantndblk = v->size - nufs_cgbase(v, cg);

			if (wantndblk > v->fpg)
				wantndblk = v->fpg;
			if (gotncyl != wantncyl)
				problem(&f, "cylinder group %d: cg_ncyl is %d,"
				    " expected %d", cg, gotncyl, wantncyl);
			if (gotniblk != v->ipg)
				problem(&f, "cylinder group %d: cg_niblk is %d,"
				    " expected %d", cg, gotniblk, v->ipg);
			if (gotcgx != cg)
				problem(&f, "cylinder group %d: cg_cgx is %d",
				    cg, gotcgx);
			if (ndblk != wantndblk)
				problem(&f, "cylinder group %d: cg_ndblk is %d,"
				    " expected %d", cg, ndblk, wantndblk);
		}
		CHECK32("cg_cs.cs_ndir", CG_CS + 0, ndir);
		CHECK32("cg_cs.cs_nbfree", CG_CS + 4, nbfree);
		CHECK32("cg_cs.cs_nifree", CG_CS + 8, nifree);
		CHECK32("cg_cs.cs_nffree", CG_CS + 12, nffree);
		for (i = 1; i < v->frag; i++)
			CHECK32("cg_frsum", CG_FRSUM + 4 * i, frsum[i]);
		for (i = 0; i < NUFS_MAXCPG; i++)
			CHECK32("cg_btot", CG_BTOT + 4 * i, btot[i]);
		for (i = 0; i < NUFS_MAXCPG * NUFS_NRPOS; i++) {
			int got = (int16_t)nufs_get16(v, v->cgbuf, CG_B + 2 * i);
			int want = bpos[i / NUFS_NRPOS][i % NUFS_NRPOS];

			if (got != want) {
				problem(&f, "cylinder group %d: cg_b[%d][%d]"
				    " is %d, counted %d", cg,
				    i / NUFS_NRPOS, i % NUFS_NRPOS, got, want);
				if (fix) {
					nufs_put16(v, v->cgbuf, CG_B + 2 * i,
					    (uint16_t)want);
					v->dirty_cg = 1;
					f.fixed++;
				}
			}
		}

		/* The fs_cs summary array must mirror the cg. */
		{
			static const char *names[] = { "cs_ndir", "cs_nbfree",
			    "cs_nifree", "cs_nffree" };
			int want[4];
			int k;

			want[0] = ndir; want[1] = nbfree;
			want[2] = nifree; want[3] = nffree;
			for (k = 0; k < 4; k++) {
				int32_t got = (int32_t)nufs_get32(v, v->csum,
				    cg * 16 + 4 * k);

				if (got != want[k]) {
					problem(&f, "fs_cs[%d].%s is %d,"
					    " counted %d", cg, names[k], got,
					    want[k]);
					if (fix) {
						nufs_put32(v, v->csum,
						    cg * 16 + 4 * k,
						    (uint32_t)want[k]);
						v->dirty_csum = 1;
						f.fixed++;
					}
				}
			}
		}
		tot_ndir += ndir;
		tot_nbfree += nbfree;
		tot_nifree += nifree;
		tot_nffree += nffree;
	}

	{
		static const char *names[] = { "cs_ndir", "cs_nbfree",
		    "cs_nifree", "cs_nffree" };
		int32_t want[4];
		int k;

		want[0] = tot_ndir; want[1] = tot_nbfree;
		want[2] = tot_nifree; want[3] = tot_nffree;
		for (k = 0; k < 4; k++) {
			int32_t got = (int32_t)nufs_get32(v, v->sb,
			    FS_CSTOTAL + 4 * k);

			if (got != want[k]) {
				problem(&f, "fs_cstotal.%s is %d, counted %d",
				    names[k], got, want[k]);
				if (fix) {
					nufs_put32(v, v->sb, FS_CSTOTAL + 4 * k,
					    (uint32_t)want[k]);
					v->dirty_sb = 1;
					f.fixed++;
				}
			}
		}
	}

	{
		/* fs_dsize must account for every fragment not held by metadata. */
		int32_t got = (int32_t)nufs_get32(v, v->sb, FS_DSIZE);
		int32_t want = v->size;

		for (cg = 0; cg < v->ncg; cg++) {
			int lo, hi;

			meta_range(v, cg, &lo, &hi);
			want -= hi - lo;
		}
		if (got != want) {
			problem(&f, "fs_dsize is %d, counted %d", got, want);
			if (fix) {
				nufs_put32(v, v->sb, FS_DSIZE, (uint32_t)want);
				v->dirty_sb = 1;
				f.fixed++;
			}
		}
	}

	if (v->sb[FS_STATE] != NUFS_STATE_CLEAN)
		problem(&f, "fs_state is %d, not clean", v->sb[FS_STATE]);

	printf("%d problem%s found", f.problems, f.problems == 1 ? "" : "s");
	if (fix)
		printf(", %d fixed", f.fixed);
	putchar('\n');
	if (fix && f.fixed > 0) {
		v->sb[FS_FMOD] = 0;
		v->sb[FS_STATE] = NUFS_STATE_CLEAN;
		v->dirty_sb = 1;
		if (nufs_flush(v) != 0)
			rc = -1;
	}
	if (f.problems > 0 && !fix)
		rc = 1;

	free(f.claimed);
	free(f.dup);
	free(f.used);
	free(f.isdir);
	free(f.ondisk_nlink);
	free(f.refs);
	free(f.parent);
	return rc;
}
