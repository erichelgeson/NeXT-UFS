/*
 * nextufs-fuse - mount a NeXT (4.3BSD) UFS volume as a normal directory.
 *
 *	nextufs-fuse [-o ro,force,part=a,...] <image> <mountpoint>
 *
 * The library underneath is path-based and holds one cylinder-group buffer,
 * one stdio handle and one error slot, so nothing here is reentrant: every
 * operation takes one mutex and libfuse is forced single-threaded.
 *
 * A file handle is the inode number and nothing else. The library rewrites an
 * inode on almost every operation, so a cached copy would go stale as soon as
 * the same file was touched by another path; re-reading it is one 128-byte
 * read, cheaper than the path walk that found it.
 */
#include <fuse.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

/* Last: it defines unprefixed DIR_*, DI_*, FS_*, CG_* and P_* constants. */
#include "nextufs.h"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE	(1 << 0)
#endif

#define NX_PATHMAX	1024		/* what src/path.c can hold */

static struct nufs		*V;
static pthread_mutex_t		 big = PTHREAD_MUTEX_INITIALIZER;
static int			 readonly;
static int			 syncwrites;
static int			 marked;	/* the volume is marked dirty */
static int			 ioerror;	/* something failed with EIO */

#define LOCK()		pthread_mutex_lock(&big)
#define UNLOCK()	pthread_mutex_unlock(&big)

/* Turn the library's last failure into an errno. */
static int
nx_fail(void)
{
	int e = V->errnum;

	if (e == 0)
		e = EIO;
	if (e == EIO)
		ioerror = 1;
	return -e;
}

static int
nx_begin(const char *path)
{
	V->errnum = 0;
	if (path != NULL && strlen(path) >= NX_PATHMAX)
		return -ENAMETOOLONG;
	return 0;
}

static uint16_t
ctx_uid(void)
{
	struct fuse_context *c = fuse_get_context();

	return c != NULL && c->uid <= 0xffff ? (uint16_t)c->uid : 0;
}

static uint16_t
ctx_gid(void)
{
	struct fuse_context *c = fuse_get_context();

	return c != NULL && c->gid <= 0xffff ? (uint16_t)c->gid : 0;
}

static void
fill_stat(struct stat *st, const struct nufs_dinode *d)
{
	memset(st, 0, sizeof(*st));
	st->st_ino = d->ino;
	st->st_mode = d->mode;
	st->st_nlink = (nlink_t)(d->nlink > 0 ? d->nlink : 0);
	st->st_uid = d->uid;
	st->st_gid = d->gid;
	st->st_size = (off_t)d->size;
	st->st_blksize = V->bsize;
	/* di_blocks counts fs_fsize fragments; st_blocks counts 512 bytes. */
	st->st_blocks = ((off_t)(d->blocks >> V->fsbtodb) * V->fsize) / 512;
	st->st_atime = (time_t)d->atime;
	st->st_mtime = (time_t)d->mtime;
	st->st_ctime = (time_t)d->ctime;
	if ((d->mode & 0170000) == 0020000 || (d->mode & 0170000) == 0060000)
		st->st_rdev = makedev((unsigned)(d->db[0] >> 8) & 0xff,
		    (unsigned)d->db[0] & 0xff);
}

/* Resolve the inode an operation works on: the open handle, or the path. */
static int
node_of(const char *path, struct fuse_file_info *fi, struct nufs_dinode *d)
{
	if (fi != NULL && fi->fh != 0) {
		if (nufs_inode_read(V, (uint32_t)fi->fh, d) != 0)
			return nx_fail();
		return 0;
	}
	if (nufs_lookup_nofollow(V, path, d) != 0)
		return nx_fail();
	return 0;
}

/* --- operations --------------------------------------------------------- */

static void *
nx_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	(void)conn;
	cfg->use_ino = 1;
	cfg->nullpath_ok = 0;
	/*
	 * The kernel does not notice that link() or unlink() changed a link
	 * count, so a cached stat would report the old one. Attributes are one
	 * inode read away; names still get the usual entry cache.
	 */
	cfg->attr_timeout = 0;
	if (!readonly && nufs_mark(V, NUFS_STATE_DIRTY) == 0)
		marked = 1;
	return NULL;
}

static void
nx_destroy(void *arg)
{
	(void)arg;
	LOCK();
	nufs_flush(V);
	if (marked && !ioerror)
		nufs_mark(V, NUFS_STATE_CLEAN);
	nufs_sync(V);
	nufs_close(V);
	V = NULL;
	UNLOCK();
}

static int
nx_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
	struct nufs_dinode d;
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) == 0 && (rc = node_of(path, fi, &d)) == 0)
		fill_stat(st, &d);
	UNLOCK();
	return rc;
}

struct dirctx {
	void			*buf;
	fuse_fill_dir_t		 filler;
	enum fuse_readdir_flags	 flags;
};

static int
dir_cb(void *arg, uint32_t ino, const char *name, int namlen)
{
	struct dirctx *c = arg;
	struct nufs_dinode d;
	enum fuse_fill_dir_flags fill = 0;
	struct stat st;

	(void)namlen;
	memset(&st, 0, sizeof(st));
	/* 4.3BSD directory entries carry no type, so each inode is read. */
	if (nufs_inode_read(V, ino, &d) == 0) {
		if ((c->flags & FUSE_READDIR_PLUS) != 0) {
			fill_stat(&st, &d);
			fill = FUSE_FILL_DIR_PLUS;
		} else {
			st.st_ino = ino;
			st.st_mode = d.mode;
		}
	} else {
		V->errnum = 0;
		st.st_ino = ino;
	}
	return c->filler(c->buf, name, &st, 0, fill) != 0 ? 1 : 0;
}

static int
nx_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
    struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	struct nufs_dinode d;
	struct dirctx c;
	int rc;

	(void)off;
	LOCK();
	if ((rc = nx_begin(path)) != 0 || (rc = node_of(path, fi, &d)) != 0)
		goto out;
	c.buf = buf;
	c.filler = filler;
	c.flags = flags;
	if (nufs_readdir(V, &d, dir_cb, &c) != 0)
		rc = nx_fail();
out:
	UNLOCK();
	return rc;
}

static int
nx_opendir(const char *path, struct fuse_file_info *fi)
{
	struct nufs_dinode d;
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) == 0) {
		if (nufs_lookup_nofollow(V, path, &d) != 0)
			rc = nx_fail();
		else if ((d.mode & 0170000) != 0040000)
			rc = -ENOTDIR;
		else
			fi->fh = d.ino;
	}
	UNLOCK();
	return rc;
}

static int
nx_readlink(const char *path, char *buf, size_t size)
{
	char link[NX_PATHMAX];
	struct nufs_dinode d;
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) != 0)
		goto out;
	if (nufs_lookup_nofollow(V, path, &d) != 0)
		rc = nx_fail();
	else if ((d.mode & 0170000) != 0120000)
		rc = -EINVAL;
	else if (nufs_readlink(V, &d, link, sizeof(link)) != 0)
		rc = nx_fail();
	else
		snprintf(buf, size, "%s", link);
out:
	UNLOCK();
	return rc;
}

static int
nx_open(const char *path, struct fuse_file_info *fi)
{
	struct nufs_dinode d;
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) != 0)
		goto out;
	if (nufs_lookup_nofollow(V, path, &d) != 0) {
		rc = nx_fail();
		goto out;
	}
	fi->fh = d.ino;
	if ((fi->flags & O_TRUNC) != 0 && nufs_truncate(V, &d, 0) != 0)
		rc = nx_fail();
out:
	UNLOCK();
	return rc;
}

static int
nx_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	struct nufs_dinode d;
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) != 0)
		goto out;
	if (nufs_create(V, path, (uint16_t)(mode & 07777), ctx_uid(), ctx_gid(),
	    &d) != 0)
		rc = nx_fail();
	else
		fi->fh = d.ino;
out:
	UNLOCK();
	return rc;
}

static int
nx_mknod(const char *path, mode_t mode, dev_t dev)
{
	uint32_t rdev = 0;
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) != 0)
		goto out;
	if (S_ISCHR(mode) || S_ISBLK(mode)) {
		if (major(dev) > 0xff || minor(dev) > 0xff) {
			rc = -EINVAL;		/* NeXT dev_t is two bytes */
			goto out;
		}
		rdev = (uint32_t)((major(dev) << 8) | minor(dev));
	}
	if (nufs_mknod(V, path, (uint16_t)(mode & 0177777), ctx_uid(),
	    ctx_gid(), rdev, NULL) != 0)
		rc = nx_fail();
out:
	UNLOCK();
	return rc;
}

static int
nx_read(const char *path, char *buf, size_t size, off_t off,
    struct fuse_file_info *fi)
{
	struct nufs_dinode d;
	long n;
	int rc;

	if (size > (size_t)INT_MAX)
		size = (size_t)INT_MAX;
	LOCK();
	if ((rc = nx_begin(path)) != 0 || (rc = node_of(path, fi, &d)) != 0)
		goto out;
	n = nufs_file_read(V, &d, buf, off, (long)size);
	rc = n < 0 ? nx_fail() : (int)n;
out:
	UNLOCK();
	return rc;
}

static int
nx_write(const char *path, const char *buf, size_t size, off_t off,
    struct fuse_file_info *fi)
{
	struct nufs_dinode d;
	long n;
	int rc;

	if (size > (size_t)INT_MAX)
		size = (size_t)INT_MAX;
	LOCK();
	if ((rc = nx_begin(path)) != 0 || (rc = node_of(path, fi, &d)) != 0)
		goto out;
	if (fi != NULL && (fi->flags & O_APPEND) != 0)
		off = (off_t)d.size;
	n = nufs_file_write(V, &d, buf, off, (long)size);
	if (n < 0)
		rc = nx_fail();
	else if (syncwrites && nufs_flush(V) != 0)
		rc = nx_fail();
	else
		rc = (int)n;
out:
	UNLOCK();
	return rc;
}

static int
nx_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	struct nufs_dinode d;
	int rc;

	if (size < 0)
		return -EINVAL;
	LOCK();
	if ((rc = nx_begin(path)) == 0 && (rc = node_of(path, fi, &d)) == 0 &&
	    nufs_truncate(V, &d, (uint64_t)size) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static int
nx_unlink(const char *path)
{
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) == 0 && nufs_unlink(V, path) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static int
nx_rmdir(const char *path)
{
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) == 0 && nufs_rmdir(V, path) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static int
nx_mkdir(const char *path, mode_t mode)
{
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) == 0 &&
	    nufs_mkdir(V, path, (uint16_t)(mode & 07777), ctx_uid(),
	    ctx_gid()) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static int
nx_symlink(const char *target, const char *path)
{
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) == 0 &&
	    nufs_symlink(V, target, path, ctx_uid(), ctx_gid()) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static int
nx_link(const char *existing, const char *path)
{
	int rc;

	LOCK();
	if ((rc = nx_begin(path)) == 0 && (rc = nx_begin(existing)) == 0 &&
	    nufs_link(V, existing, path) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static int
nx_rename(const char *from, const char *to, unsigned int flags)
{
	int rc;

	if ((flags & ~(unsigned)RENAME_NOREPLACE) != 0)
		return -EINVAL;			/* no RENAME_EXCHANGE */
	LOCK();
	if ((rc = nx_begin(from)) == 0 && (rc = nx_begin(to)) == 0 &&
	    nufs_rename(V, from, to,
	    (flags & RENAME_NOREPLACE) != 0 ? NUFS_RENAME_NOREPLACE : 0) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static int
nx_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int rc;

	(void)fi;
	LOCK();
	if ((rc = nx_begin(path)) == 0 &&
	    nufs_chmod(V, path, (uint16_t)(mode & 07777)) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static int
nx_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
	struct nufs_dinode d;
	int rc;

	(void)fi;
	if ((uid != (uid_t)-1 && uid > 0xffff) ||
	    (gid != (gid_t)-1 && gid > 0xffff))
		return -EOVERFLOW;		/* NeXT ids are 16 bits */
	LOCK();
	if ((rc = nx_begin(path)) != 0)
		goto out;
	if (nufs_lookup_nofollow(V, path, &d) != 0) {
		rc = nx_fail();
		goto out;
	}
	if (nufs_chown(V, path,
	    uid == (uid_t)-1 ? d.uid : (uint16_t)uid,
	    gid == (gid_t)-1 ? d.gid : (uint16_t)gid) != 0)
		rc = nx_fail();
out:
	UNLOCK();
	return rc;
}

static int
nx_utimens(const char *path, const struct timespec tv[2],
    struct fuse_file_info *fi)
{
	uint32_t at, mt;
	const uint32_t *ap = &at, *mp = &mt;
	int rc;

	(void)fi;
	at = mt = (uint32_t)time(NULL);
	if (tv != NULL) {
		if (tv[0].tv_nsec == UTIME_OMIT)
			ap = NULL;
		else if (tv[0].tv_nsec != UTIME_NOW)
			at = (uint32_t)tv[0].tv_sec;
		if (tv[1].tv_nsec == UTIME_OMIT)
			mp = NULL;
		else if (tv[1].tv_nsec != UTIME_NOW)
			mt = (uint32_t)tv[1].tv_sec;
	}
	LOCK();
	if ((rc = nx_begin(path)) == 0 && nufs_utimes(V, path, ap, mp) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static int
nx_statfs(const char *path, struct statvfs *st)
{
	long long nbfree, nffree, nifree, avail, reserved;

	(void)path;
	LOCK();
	nbfree = (int32_t)nufs_get32(V, V->sb, FS_CSTOTAL + 4);
	nifree = (int32_t)nufs_get32(V, V->sb, FS_CSTOTAL + 8);
	nffree = (int32_t)nufs_get32(V, V->sb, FS_CSTOTAL + 12);

	memset(st, 0, sizeof(*st));
	st->f_bsize = (unsigned long)V->bsize;
	st->f_frsize = (unsigned long)V->fsize;
	st->f_blocks = (fsblkcnt_t)V->dsize;
	st->f_bfree = (fsblkcnt_t)(nbfree * V->frag + nffree);
	reserved = (long long)V->dsize * V->minfree / 100;
	avail = nbfree * V->frag + nffree - reserved;
	st->f_bavail = (fsblkcnt_t)(avail > 0 ? avail : 0);
	st->f_files = (fsfilcnt_t)((long long)V->ncg * V->ipg);
	st->f_ffree = st->f_favail = (fsfilcnt_t)nifree;
	st->f_namemax = NUFS_MAXNAMLEN;
	UNLOCK();
	return 0;
}

static int
nx_flush(const char *path, struct fuse_file_info *fi)
{
	int rc = 0;

	(void)path;
	(void)fi;
	LOCK();
	V->errnum = 0;
	if (nufs_flush(V) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static int
nx_release(const char *path, struct fuse_file_info *fi)
{
	return nx_flush(path, fi);
}

static int
nx_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	int rc = 0;

	(void)path;
	(void)datasync;
	(void)fi;
	LOCK();
	V->errnum = 0;
	if (nufs_sync(V) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

/*
 * There is no permission model here: the volume's own uid, gid and mode are
 * shown, and the kernel checks nothing unless the mount asked it to with
 * -o default_permissions. So this answers "does it exist", as vfat does.
 */
static int
nx_access(const char *path, int mask)
{
	struct nufs_dinode d;
	int rc;

	(void)mask;
	LOCK();
	if ((rc = nx_begin(path)) == 0 && nufs_lookup_nofollow(V, path, &d) != 0)
		rc = nx_fail();
	UNLOCK();
	return rc;
}

static const struct fuse_operations nx_ops = {
	.init		= nx_init,
	.destroy	= nx_destroy,
	.getattr	= nx_getattr,
	.readdir	= nx_readdir,
	.opendir	= nx_opendir,
	.readlink	= nx_readlink,
	.open		= nx_open,
	.create		= nx_create,
	.mknod		= nx_mknod,
	.read		= nx_read,
	.write		= nx_write,
	.truncate	= nx_truncate,
	.unlink		= nx_unlink,
	.rmdir		= nx_rmdir,
	.mkdir		= nx_mkdir,
	.symlink	= nx_symlink,
	.link		= nx_link,
	.rename		= nx_rename,
	.chmod		= nx_chmod,
	.chown		= nx_chown,
	.utimens	= nx_utimens,
	.statfs		= nx_statfs,
	.flush		= nx_flush,
	.release	= nx_release,
	.fsync		= nx_fsync,
	.access		= nx_access,
};

/* --- options ------------------------------------------------------------ */

struct opts {
	char	*image;
	char	*part;
	int	 ro;
	int	 force;
	int	 sync;
	int	 help;
};

enum { KEY_RO, KEY_HELP };

#define NX_OPT(t, m)	{ t, offsetof(struct opts, m), 1 }

static const struct fuse_opt nx_optspec[] = {
	{ "-p %s",	offsetof(struct opts, part), 0 },
	{ "part=%s",	offsetof(struct opts, part), 0 },
	NX_OPT("force", force),
	NX_OPT("sync", sync),
	FUSE_OPT_KEY("ro", KEY_RO),
	FUSE_OPT_KEY("-h", KEY_HELP),
	FUSE_OPT_KEY("--help", KEY_HELP),
	FUSE_OPT_END
};

static void
usage(void)
{
	fprintf(stderr,
"usage: nextufs-fuse [-o option,...] <image> <mountpoint>\n"
"\n"
"  -o ro            mount read-only\n"
"  -o part=a        pick a partition: a NeXT label letter, or an A/UX\n"
"                   Apple Partition Map number or name\n"
"  -o force         mount a volume that was not cleanly unmounted\n"
"  -o sync          flush after every write\n"
"  -o uid=,gid=,umask=   show every file as this owner and mode\n"
"  -o allow_other, -o default_permissions   as for any FUSE mount\n"
"\n"
"unmount with: fusermount3 -u <mountpoint>\n");
}

static int
nx_optproc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	struct opts *o = data;

	(void)outargs;
	switch (key) {
	case FUSE_OPT_KEY_NONOPT:
		if (o->image == NULL) {
			/* libfuse chdir()s to / when it daemonises. */
			o->image = realpath(arg, NULL);
			if (o->image == NULL) {
				fprintf(stderr, "nextufs-fuse: %s: %s\n", arg,
				    strerror(errno));
				return -1;
			}
			return 0;		/* consumed: not a mountpoint */
		}
		return 1;			/* the mountpoint, keep it */
	case KEY_RO:
		o->ro = 1;
		return 1;			/* the kernel wants it too */
	case KEY_HELP:
		o->help = 1;
		return 1;
	}
	return 1;
}

int
main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct opts o;
	char err[256], fsname[NX_PATHMAX + 64];
	int rc;

	memset(&o, 0, sizeof(o));
	if (fuse_opt_parse(&args, &o, nx_optspec, nx_optproc) != 0)
		return 1;
	if (o.help) {
		usage();
		fuse_opt_add_arg(&args, "-ho");
		fuse_main(args.argc, args.argv, &nx_ops, NULL);
		return 0;
	}
	if (o.image == NULL) {
		usage();
		return 1;
	}
	readonly = o.ro;
	syncwrites = o.sync;

	V = nufs_open(o.image, o.part, !readonly, err, sizeof(err));
	if (V == NULL) {
		fprintf(stderr, "nextufs-fuse: %s: %s\n", o.image, err);
		return 1;
	}
	/* A/UX has no clean marker to check; see nufs_mark(). */
	if (!readonly && !V->aux && !nufs_is_clean(V) && !o.force) {
		fprintf(stderr, "nextufs-fuse: %s was not cleanly unmounted.\n"
		    "Run `nextufs %s fsck -y`, or mount it with -o force or "
		    "-o ro.\n", o.image, o.image);
		nufs_close(V);
		return 1;
	}

	/* One cylinder-group buffer and one FILE*: one request at a time. */
	fuse_opt_add_arg(&args, "-s");
	snprintf(fsname, sizeof(fsname), "-ofsname=%s,subtype=nextufs",
	    o.image);
	fuse_opt_insert_arg(&args, 1, fsname);

	rc = fuse_main(args.argc, args.argv, &nx_ops, NULL);
	if (V != NULL)				/* init never ran */
		nufs_close(V);
	fuse_opt_free_args(&args);
	free(o.image);
	return rc;
}
