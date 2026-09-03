/*
 * nextufs - read/write NeXT (4.3BSD "old format") UFS volumes from a host.
 *
 * Structure layouts and constants come from NeXT's own headers, extracted
 * from an OPENSTEP 4.2 disk image; see docs/next-headers/.
 *
 * Everything on disk is a 32-bit signed quantity in the volume's byte order
 * (big-endian on m68k NeXT hardware, little-endian on NeXTSTEP/Intel).
 * Disk addresses ("frags") are counted in fs_fsize units from the start of
 * the partition.
 */
#ifndef NEXTUFS_H
#define NEXTUFS_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* ---- on-disk constants (bsd/ufs/fs.h, bsd/ufs/inode.h, bsd/ufs/fsdir.h) -- */
#define NUFS_BBSIZE		8192
#define NUFS_SBOFF		8192		/* superblock byte offset */
#define NUFS_SBSIZE		8192
#define NUFS_FS_MAGIC		0x00011954
#define NUFS_CG_MAGIC		0x00090255
#define NUFS_ROOTINO		2
#define NUFS_LOSTFOUNDINO	3
#define NUFS_NRPOS		8		/* rotational positions */
#define NUFS_MAXIPG		2048		/* max inodes per cyl group */
#define NUFS_MAXCPG		32		/* max cylinders per group */
#define NUFS_MAXMNTLEN		512
#define NUFS_MAXCSBUFS		32
#define NUFS_MAXFRAG		8
#define NUFS_NDADDR		12
#define NUFS_NIADDR		3
#define NUFS_MAXFASTLINK	60		/* (NDADDR+NIADDR)*4 */
#define NUFS_DINODE_SIZE	128
#define NUFS_DIRBLKSIZ		1024		/* the largest; see nufs->dirblksiz */
#define NUFS_MAXNAMLEN		255
#define NUFS_IC_FASTLINK	0x0001		/* di_flags: link is inline */

/* fs_state (NeXT replaces 4.4BSD's fs_clean) */
#define NUFS_STATE_CLEAN	1
#define NUFS_STATE_DIRTY	2
#define NUFS_STATE_CORRUPTED	3

/* The same byte on SunOS is fs_clean, and it counts the other way: zero is
 * the volume that needs checking. It is only believed when the fs_state
 * word agrees, which is why FSOKAY exists. From SunOS 4.1.4 sys/ufs/fs.h. */
#define NUFS_FSACTIVE		0x00		/* in use, may not be clean */
#define NUFS_FSCLEAN		0x01		/* clean and unmounted */
#define NUFS_FSSTABLE		0x02		/* clean, still mounted */
#define NUFS_FSOKAY		0x7c269d38	/* fs_state = FSOKAY - fs_time */

/* fs_optim */
#define NUFS_OPTTIME		0
#define NUFS_OPTSPACE		1

/* ---- superblock field byte offsets (struct fs) ---------------------------
 * Kept as offsets rather than a packed struct: the layout is fixed forever
 * and this keeps every access byte-order explicit.
 */
#define FS_SBLKNO	8
#define FS_CBLKNO	12
#define FS_IBLKNO	16
#define FS_DBLKNO	20
#define FS_CGOFFSET	24
#define FS_CGMASK	28
#define FS_TIME		32
#define FS_SIZE		36
#define FS_DSIZE	40
#define FS_NCG		44
#define FS_BSIZE	48
#define FS_FSIZE	52
#define FS_FRAG		56
#define FS_MINFREE	60
#define FS_ROTDELAY	64
#define FS_RPS		68
#define FS_BMASK	72
#define FS_FMASK	76
#define FS_BSHIFT	80
#define FS_FSHIFT	84
#define FS_MAXCONTIG	88
#define FS_MAXBPG	92
#define FS_FRAGSHIFT	96
#define FS_FSBTODB	100
#define FS_SBSIZE	104
#define FS_CSMASK	108
#define FS_CSSHIFT	112
#define FS_NINDIR	116
#define FS_INOPB	120
#define FS_NSPF		124
#define FS_OPTIM	128
#define FS_NPSECT	132		/* sectors/track including spares */
#define FS_INTERLEAVE	136
#define FS_TRACKSKEW	140
#define FS_CSADDR	152
#define FS_CSSIZE	156
#define FS_CGSIZE	160
#define FS_NTRAK	164
#define FS_NSECT	168
#define FS_SPC		172
#define FS_NCYL		176
#define FS_CPG		180
#define FS_IPG		184
#define FS_FPG		188
#define FS_CSTOTAL	192		/* struct csum: ndir,nbfree,nifree,nffree */
#define FS_FMOD		208		/* char */
#define FS_STATE	209		/* char */
#define FS_RONLY	210		/* char */
#define FS_FLAGS	211		/* char */
#define FS_FSMNT	212		/* char[512] */
#define FS_CGROTOR	724
#define FS_CSP		728		/* long[32], in-core only */
#define FS_CPC		856
#define FS_POSTBL	860		/* short[32][8] */
#define FS_SPARECON	1116		/* long[56] */
#define FS_SUN_STATE	(FS_SPARECON + 55 * 4)	/* dynamic-layout fs_state */
#define FS_POSTBLFORMAT	1356		/* only in the dynamic layout */
#define FS_NRPOS	1360
#define FS_POSTBLOFF	1364
#define FS_ROTBLOFF	1368
#define FS_MAGIC	1372
#define FS_ROTBL	1376

/* fs_postblformat. The old layout has no such field; these bytes are
 * rotation table data there, so the cylinder group is what settles it. */
#define NUFS_42POSTBLFMT	(-1)
#define NUFS_DYNAMICPOSTBLFMT	1

/* ---- cylinder group field byte offsets (struct cg; the old static form) -- */
#define CG_TIME		8
#define CG_CGX		12
#define CG_NCYL		16		/* short */
#define CG_NIBLK	18		/* short */
#define CG_NDBLK	20
#define CG_CS		24		/* struct csum */
#define CG_ROTOR	40
#define CG_FROTOR	44
#define CG_IROTOR	48
#define CG_FRSUM	52		/* long[8] */
#define CG_BTOT		84		/* long[32] */
#define CG_B		212		/* short[32][8] */
#define CG_IUSED	724		/* char[256] */
#define CG_MAGIC	980
#define CG_FREE		984		/* u_char[], free frag bitmap */

/* The dynamic form, which SunOS and every later BSD write. cg_magic moves to
 * the front and the four maps move to offsets the header itself carries, so
 * a group is only as big as its own geometry needs. Everything between
 * cg_time and cg_frsum stays where it was. */
#define CG_DYN_MAGIC	4
#define CG_DYN_BTOTOFF	84
#define CG_DYN_BOFF	88
#define CG_DYN_IUSEDOFF	92
#define CG_DYN_FREEOFF	96
#define CG_DYN_NEXTFREEOFF 100

/* ---- inode field byte offsets (struct icommon) -------------------------- */
#define DI_MODE		0		/* u_short */
#define DI_NLINK	2		/* short */
#define DI_UID		4		/* u_short */
#define DI_GID		6		/* u_short */
#define DI_SIZE		8		/* quad: high word first on big-endian */
#define DI_ATIME	16
#define DI_MTIME	24
#define DI_CTIME	32
#define DI_DB		40		/* long[12] */
#define DI_IB		88		/* long[3] */
#define DI_SYMLINK	40		/* char[60], when DI_FLAGS & FASTLINK */
#define DI_FLAGS	100
#define DI_BLOCKS	104
#define DI_GEN		108

/* ---- A/UX -----------------------------------------------------------------
 * Apple's UNIX uses the same 4.3BSD filesystem and keeps its Macintosh
 * metadata in the nine long words 4.3BSD leaves spare. A/UX's own
 * <sys/xstat.h> exposes them as `long st_xinfo[XINFOSIZ]` with XINFOSIZ 9,
 * overlaid by a union: one shape for a file, another for a directory. The
 * nine are the high half of the size, the three timestamp spares, di_flags
 * and di_spare[4]. See NeXT-UFS-Spec.md §12.3.
 */
#define DI_FDRLEN	DI_SIZE		/* file: resource fork length */
#define DI_VALENCE	DI_SIZE		/* directory: entry count. Same word. */
#define DI_FDTYPE	20		/* ic_atspare: Finder type */
#define DI_FDCREATOR	28		/* ic_mtspare: Finder creator */
#define DI_FDLOCATION	36		/* ic_ctspare: icon position */
#define DI_FDMDDAT	DI_FLAGS	/* file: Finder modification date */
#define DI_AUXID	DI_FLAGS	/* directory: Macintosh directory ID */
#define DI_FDFLDR	112		/* file: Finder folder number */
#define DI_FDFLAGS	114		/* file: Finder flags, u_short */
#define DI_FDSCRIPT	116		/* file: script, xflags and vf_flags */
#define DI_FDCRDAT	120		/* file: creation date. dir: frMdDat */
#define DI_FDLEN	124		/* file: data fork length */
#define NUFS_MACROOTID	2		/* HFS fsRtDirID: A/UX gives its root
					 * directory the same number */
#define NUFS_FDALIAS	0x8000		/* fdFlags: the file is an alias */
#define NUFS_FDINITED	0x0100		/* fdFlags: the Finder has seen it */

/* ---- directory entry ---------------------------------------------------- */
#define DIR_INO		0		/* u_long */
#define DIR_RECLEN	4		/* u_short */
#define DIR_NAMLEN	6		/* u_short */
#define DIR_NAME	8
#define NUFS_DIRSIZ(namlen)	(8 + (((namlen) + 1 + 3) & ~3))

/* ---- NeXT disk label (bsd/dev/disk_label.h, bsd/sys/disktab.h) ----------
 * Laid out for m68k 2-byte alignment, which is what is actually on disk.
 */
#define DL_VERSION	0		/* "dlV3" / "dlV2" / "NeXT" */
#define DL_LABEL_BLKNO	4
#define DL_SIZE		8
#define DL_LABEL	12		/* char[24] */
#define DL_FLAGS	36
#define DL_TAG		40
#define DL_DT		44		/* struct disktab */
#define DT_NAME		(DL_DT + 0)	/* char[24] */
#define DT_TYPE		(DL_DT + 24)	/* char[24] */
#define DT_SECSIZE	(DL_DT + 48)
#define DT_NTRACKS	(DL_DT + 52)
#define DT_NSECTORS	(DL_DT + 56)
#define DT_NCYLINDERS	(DL_DT + 60)
#define DT_RPM		(DL_DT + 64)
#define DT_FRONT	(DL_DT + 68)	/* short */
#define DT_BACK		(DL_DT + 70)	/* short */
#define DT_NGROUPS	(DL_DT + 72)	/* short */
#define DT_AG_SIZE	(DL_DT + 74)	/* short */
#define DT_AG_ALTS	(DL_DT + 76)	/* short */
#define DT_AG_OFF	(DL_DT + 78)	/* short */
#define DT_BOOT0_BLKNO	(DL_DT + 80)	/* long[2] */
#define DT_BOOTFILE	(DL_DT + 88)	/* char[24] */
#define DT_HOSTNAME	(DL_DT + 112)	/* char[32] */
#define DT_ROOTPART	(DL_DT + 144)	/* char */
#define DT_RWPART	(DL_DT + 145)	/* char */
#define DT_PARTITIONS	(DL_DT + 146)	/* struct partition[8] */
#define NUFS_NPART	8
#define NUFS_PARTSIZE	46		/* sizeof(struct partition) on disk */
#define P_BASE		0
#define P_SIZE		4
#define P_BSIZE		8		/* short */
#define P_FSIZE		10		/* short */
#define P_OPT		12		/* char */
#define P_CPG		14		/* short */
#define P_DENSITY	16		/* short */
#define P_MINFREE	18		/* char */
#define P_NEWFS		19		/* char */
#define P_MOUNTPT	20		/* char[16] */
#define P_AUTOMNT	36		/* char */
#define P_TYPE		37		/* char[8] */
#define DL_UN		558		/* v3 checksum lives here */
#define DL_V3_CHECKSUM	558
#define DL_CHECKSUM	7238
#define DL_UNINIT	0x80000000u

/* ------------------------------------------------------------------------ */

struct nufs_part {
	int	base;			/* sectors, relative to the front porch */
	int	size;			/* sectors */
	int	bsize, fsize;
	int	cpg, density, minfree;
	char	opt, newfs, automnt;
	char	mountpt[17];
	char	type[9];
};

struct nufs_label {
	uint8_t		raw[NUFS_SBSIZE];
	int		valid;
	char		version[5];
	char		name[25];		/* dl_label */
	char		drive[25];		/* d_name */
	char		drvtype[25];		/* d_type */
	int		secsize, ntracks, nsectors, ncylinders, rpm;
	int		front, back;
	int		size;			/* dl_size, sectors */
	char		rootpart, rwpart;
	struct nufs_part part[NUFS_NPART];
};

struct nufs_dinode {
	uint32_t	ino;
	uint16_t	mode;
	int16_t		nlink;
	uint16_t	uid, gid;
	uint64_t	size;
	uint32_t	atime, mtime, ctime;
	/*
	 * SunOS stores each timestamp as a struct timeval, so the word after
	 * each second holds its microseconds. NeXT leaves those words spare
	 * and A/UX keeps Finder data in them, so this is zero on both.
	 */
	uint32_t	ausec, musec, cusec;
	int32_t		db[NUFS_NDADDR];
	int32_t		ib[NUFS_NIADDR];
	uint32_t	flags, blocks, gen;
	char		symlink[NUFS_MAXFASTLINK + 1];

	/*
	 * A/UX only, zero elsewhere. Two of these words mean one thing on a
	 * directory and another on a file, because A/UX overlays two structs
	 * on the same nine spare longs.
	 */
	uint32_t	valence;		/* directory: entry count, less
						 * . and .. . file: fdRLen, the
						 * resource fork length */
	char		fdtype[5];		/* Finder type and creator */
	char		fdcreator[5];
	uint32_t	auxid;			/* the word 4.3BSD calls di_flags:
						 * directory: Macintosh directory
						 * ID. file: fdMdDat, the Finder's
						 * modification date */
	uint32_t	fdlen;			/* file: data fork length. A file
						 * with a resource fork is stored
						 * as AppleSingle, so this is not
						 * di_size for one of those */
	uint32_t	fdcrdat;		/* file: creation date */
	uint16_t	fdflags;		/* file: Finder flags */
};

#define NUFS_MAXAPM	16

struct nufs_apm_part {
	uint32_t	start, size;		/* in blocksize units */
	char		name[33];
	char		type[33];
};

struct nufs_apm {
	int	valid;
	int	n;
	int	blocksize;
	struct nufs_apm_part part[NUFS_MAXAPM];
};
#define NUFS_APM_BLKMAX	2048

struct nufs {
	FILE		*f;
	char		*path;
	int		rw;
	int		be;			/* volume byte order */
	long long	partoff;		/* byte offset of the partition */
	int		partno;			/* -1 if opened without a label */
	struct nufs_label label;
	struct nufs_apm	apm;			/* A/UX: the Apple Partition Map */
	int		apmno;			/* map entry in use, -1 if none */
	int		aux;			/* volume carries A/UX metadata */
	int		dyncg;			/* dynamic cylinder groups */

	uint8_t		sb[NUFS_SBSIZE];
	int		dirty_sb;

	/* geometry, host order */
	int	sblkno, cblkno, iblkno, dblkno, cgoffset, cgmask;
	int	size, dsize, ncg, bsize, fsize, frag, minfree;
	int	bshift, fshift, fragshift, fsbtodb, sbsize;
	int	csmask, csshift, nindir, inopb, nspf, optim;
	int	csaddr, cssize, cgsize, ntrak, nsect, spc, ncyl, cpg, ipg, fpg;
	int	cpc, npsect, interleave, trackskew;
	int	dirblksiz;		/* directory block: fs_fsize >> fs_fsbtodb */
	int	nrpos;			/* rotational positions; 8 when static */

	uint8_t		*csum;			/* fs_cssize bytes, on-disk order */
	int		dirty_csum;

	int		cgno;			/* cylinder group in the buffer */
	uint8_t		*cgbuf;
	int		dirty_cg;
	/*
	 * Where the four maps sit inside cgbuf. Constants in the static
	 * layout; read out of the group header in the dynamic one, which is
	 * the whole of the difference. cgcpg is how many cylinders cg_btot
	 * has room for.
	 */
	int		cg_btot, cg_b, cg_iused, cg_free, cgcpg;

	char		err[256];
	int		errnum;			/* errno for the last failure */
};

/* util.c */
uint32_t nufs_get32(const struct nufs *, const void *, int off);
uint16_t nufs_get16(const struct nufs *, const void *, int off);
void	 nufs_put32(const struct nufs *, void *, int off, uint32_t v);
void	 nufs_put16(const struct nufs *, void *, int off, uint16_t v);
int	 nufs_pread(struct nufs *, void *buf, long long off, size_t n);
int	 nufs_pwrite(struct nufs *, const void *buf, long long off, size_t n);
void	 nufs_err(struct nufs *, const char *fmt, ...);
void	 nufs_errc(struct nufs *, int errnum, const char *fmt, ...);

/* label.c */
int	nufs_label_read(struct nufs *, struct nufs_label *);
uint16_t nufs_label_checksum(const uint8_t *buf, int limit);
void	nufs_label_print(const struct nufs_label *, FILE *);

/* apm.c */
int	nufs_apm_read(struct nufs *, struct nufs_apm *);
int	nufs_apm_is_unix(const struct nufs_apm_part *);
void	nufs_apm_print(const struct nufs_apm *, FILE *);

/* fs.c */
struct nufs *nufs_open(const char *path, const char *partspec, int rw, char *errbuf, size_t errlen);
void	nufs_close(struct nufs *);
int	nufs_flush(struct nufs *);
int	nufs_sync(struct nufs *);
int	nufs_mark(struct nufs *, int state);
int	nufs_is_clean(const struct nufs *);
long long nufs_fragoff(const struct nufs *, int frag);
int	nufs_read_frags(struct nufs *, int frag, int nfrags, void *buf);
int	nufs_write_frags(struct nufs *, int frag, int nfrags, const void *buf);
int	nufs_cg_load(struct nufs *, int cg);
int	nufs_cgstart(const struct nufs *, int cg);

/* alloc.c */
int	nufs_cgbase(const struct nufs *, int cg);
int	nufs_cbtocylno(const struct nufs *, int bno);
int	nufs_cbtorpos(const struct nufs *, int bno);
int	nufs_alloc_block(struct nufs *, int prefcg);
int	nufs_alloc_frags(struct nufs *, int prefcg, int nfrags);
int	nufs_free_frags(struct nufs *, int frag, int nfrags);
uint32_t nufs_alloc_inode(struct nufs *, int prefcg, int isdir);
int	nufs_free_inode(struct nufs *, uint32_t ino, int isdir);
void	nufs_touch(struct nufs *);

/* inode.c */
int	nufs_detect_aux(struct nufs *);
int	nufs_inode_read(struct nufs *, uint32_t ino, struct nufs_dinode *);
int	nufs_inode_write(struct nufs *, const struct nufs_dinode *);
int	nufs_bmap(struct nufs *, const struct nufs_dinode *, int lbn, int *frag);
long	nufs_file_read(struct nufs *, const struct nufs_dinode *, void *buf,
	    long long off, long n);
int	nufs_readlink(struct nufs *, const struct nufs_dinode *, char *buf, size_t n);

/* write.c */
int	nufs_lbn_frags(const struct nufs *, uint64_t size, int lbn);
int	nufs_tail_frags(const struct nufs *, const struct nufs_dinode *);
int	nufs_blk_frags(const struct nufs *, const struct nufs_dinode *, int lbn);
int	nufs_bmap_alloc(struct nufs *, struct nufs_dinode *, int lbn, int nfrags, int *out);
long	nufs_file_write(struct nufs *, struct nufs_dinode *, const void *buf,
	    long long off, long n);
int	nufs_truncate(struct nufs *, struct nufs_dinode *, uint64_t newsize);

/* dir.c */
typedef int (*nufs_dir_cb)(void *arg, uint32_t ino, const char *name, int namlen);
int	nufs_readdir(struct nufs *, const struct nufs_dinode *, nufs_dir_cb, void *arg);
int	nufs_dir_lookup(struct nufs *, const struct nufs_dinode *dir,
	    const char *name, uint32_t *ino);

/* dirops.c */
int	nufs_dir_add(struct nufs *, struct nufs_dinode *dir, const char *name, uint32_t ino);
int	nufs_dir_remove(struct nufs *, struct nufs_dinode *dir, const char *name);
int	nufs_lookup_parent(struct nufs *, const char *path,
	    struct nufs_dinode *parent, char *name, size_t namesz);
int	nufs_create(struct nufs *, const char *path, uint16_t mode,
	    uint16_t uid, uint16_t gid, struct nufs_dinode *out);
int	nufs_mkdir(struct nufs *, const char *path, uint16_t mode, uint16_t uid, uint16_t gid);
int	nufs_symlink(struct nufs *, const char *target, const char *path,
	    uint16_t uid, uint16_t gid);
int	nufs_unlink(struct nufs *, const char *path);
int	nufs_rmdir(struct nufs *, const char *path);
int	nufs_rename(struct nufs *, const char *from, const char *to, int flags);
#define NUFS_RENAME_NOREPLACE	1
int	nufs_link(struct nufs *, const char *existing, const char *newpath);
int	nufs_utimes(struct nufs *, const char *path, const uint32_t *atime,
	    const uint32_t *mtime);
int	nufs_mknod(struct nufs *, const char *path, uint16_t mode,
	    uint16_t uid, uint16_t gid, uint32_t rdev, struct nufs_dinode *out);
int	nufs_chmod(struct nufs *, const char *path, uint16_t mode);
int	nufs_chown(struct nufs *, const char *path, uint16_t uid, uint16_t gid);

/* mkfs.c */
int	nufs_mkfs(const char *path, long long totsect, const char *name, int quiet);

/* fsck.c */
int	nufs_fsck(struct nufs *, int fix);

/* path.c */
int	nufs_lookup(struct nufs *, const char *path, struct nufs_dinode *);
int	nufs_lookup_nofollow(struct nufs *, const char *path, struct nufs_dinode *);

#endif /* NEXTUFS_H */
