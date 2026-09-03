/*
 * Directory mutation and the file/directory/symlink lifecycle.
 *
 * Directory data is a run of 1024-byte blocks. Within a block the entries'
 * d_reclen values tile it exactly, no entry straddles a block boundary, and a
 * free slot is an entry with d_ino == 0. Both rules matter: NeXT's fsck
 * rejects a directory that breaks either.
 */
#include "nextufs.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define S_IFMT_	0170000
#define S_DIR_	0040000
#define S_REG_	0100000
#define S_LNK_	0120000
#define S_CHR_	0020000
#define S_BLK_	0060000

static int
dirsiz(int namlen)
{
	return NUFS_DIRSIZ(namlen);
}

static int
read_dirblk(struct nufs *v, const struct nufs_dinode *dir, long long off,
    uint8_t *buf)
{
	long n = nufs_file_read(v, dir, buf, off, v->dirblksiz);

	if (n != v->dirblksiz) {
		nufs_err(v, "short read of directory block at %lld", off);
		return -1;
	}
	return 0;
}

static int
write_dirblk(struct nufs *v, struct nufs_dinode *dir, long long off,
    const uint8_t *buf)
{
	if (nufs_file_write(v, dir, buf, off, v->dirblksiz) != v->dirblksiz)
		return -1;
	return 0;
}

static int
count_cb(void *arg, uint32_t ino, const char *name, int namlen)
{
	(void)ino;
	(void)namlen;
	if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0)
		(*(uint32_t *)arg)++;
	return 0;
}

/*
 * Bring an A/UX directory's valence back in line with what it now holds.
 * Counting beats adjusting by one: A/UX leaves the field zero on directories
 * its Finder has never looked at, so there is not always a count to adjust.
 */
static int
fix_valence(struct nufs *v, struct nufs_dinode *dir)
{
	uint32_t n = 0;

	if (!v->aux)
		return 0;
	if (nufs_readdir(v, dir, count_cb, &n) != 0)
		return -1;
	if (n == dir->valence)
		return 0;
	dir->valence = n;
	return nufs_inode_write(v, dir);
}

/* Lay out a fresh directory block holding a single entry filling the block. */
static void
init_dirblk(struct nufs *v, uint8_t *buf, uint32_t ino, const char *name)
{
	int namlen = (int)strlen(name);

	memset(buf, 0, v->dirblksiz);
	nufs_put32(v, buf, DIR_INO, ino);
	nufs_put16(v, buf, DIR_RECLEN, v->dirblksiz);
	nufs_put16(v, buf, DIR_NAMLEN, (uint16_t)namlen);
	memcpy(buf + DIR_NAME, name, (size_t)namlen);
}

int
nufs_dir_add(struct nufs *v, struct nufs_dinode *dir, const char *name,
    uint32_t ino)
{
	uint8_t buf[NUFS_DIRBLKSIZ];
	int need = dirsiz((int)strlen(name));
	int namlen = (int)strlen(name);
	long long off;
	uint32_t exists;

	if (namlen == 0 || namlen > NUFS_MAXNAMLEN) {
		nufs_errc(v, namlen == 0 ? EINVAL : ENAMETOOLONG,
		    "bad name length");
		return -1;
	}
	if (nufs_dir_lookup(v, dir, name, &exists) == 0) {
		nufs_errc(v, EEXIST, "\"%s\" already exists", name);
		return -1;
	}

	for (off = 0; off < (long long)dir->size; off += v->dirblksiz) {
		int o = 0;

		if (read_dirblk(v, dir, off, buf) != 0)
			return -1;
		while (o < v->dirblksiz) {
			uint32_t eino = nufs_get32(v, buf, o + DIR_INO);
			int reclen = nufs_get16(v, buf, o + DIR_RECLEN);
			int elen = nufs_get16(v, buf, o + DIR_NAMLEN);
			int used = eino == 0 ? 0 : dirsiz(elen);

			if (reclen <= 0 || o + reclen > v->dirblksiz) {
				nufs_err(v, "corrupt directory block at %lld",
				    off);
				return -1;
			}
			if (reclen - used >= need) {
				int at = o + used;

				if (used != 0)
					nufs_put16(v, buf, o + DIR_RECLEN,
					    (uint16_t)used);
				memset(buf + at, 0, (size_t)(reclen - used));
				nufs_put32(v, buf, at + DIR_INO, ino);
				nufs_put16(v, buf, at + DIR_RECLEN,
				    (uint16_t)(reclen - used));
				nufs_put16(v, buf, at + DIR_NAMLEN,
				    (uint16_t)namlen);
				memcpy(buf + at + DIR_NAME, name,
				    (size_t)namlen);
				if (write_dirblk(v, dir, off, buf) != 0)
					return -1;
				return fix_valence(v, dir);
			}
			o += reclen;
		}
	}

	/* No room anywhere: append a block. */
	init_dirblk(v, buf, ino, name);
	if (write_dirblk(v, dir, (long long)dir->size, buf) != 0)
		return -1;
	return fix_valence(v, dir);
}

int
nufs_dir_remove(struct nufs *v, struct nufs_dinode *dir, const char *name)
{
	uint8_t buf[NUFS_DIRBLKSIZ];
	long long off;

	for (off = 0; off < (long long)dir->size; off += v->dirblksiz) {
		int o = 0, prev = -1;

		if (read_dirblk(v, dir, off, buf) != 0)
			return -1;
		while (o < v->dirblksiz) {
			uint32_t eino = nufs_get32(v, buf, o + DIR_INO);
			int reclen = nufs_get16(v, buf, o + DIR_RECLEN);
			int elen = nufs_get16(v, buf, o + DIR_NAMLEN);

			if (reclen <= 0 || o + reclen > v->dirblksiz) {
				nufs_err(v, "corrupt directory block at %lld",
				    off);
				return -1;
			}
			if (eino != 0 && elen == (int)strlen(name) &&
			    memcmp(buf + o + DIR_NAME, name, (size_t)elen) == 0) {
				if (prev < 0)
					nufs_put32(v, buf, o + DIR_INO, 0);
				else
					nufs_put16(v, buf, prev + DIR_RECLEN,
					    (uint16_t)(nufs_get16(v, buf,
					    prev + DIR_RECLEN) + reclen));
				if (write_dirblk(v, dir, off, buf) != 0)
					return -1;
				return fix_valence(v, dir);
			}
			prev = o;
			o += reclen;
		}
	}
	nufs_errc(v, ENOENT, "no entry \"%s\"", name);
	return -1;
}

/* Re-point an existing entry at another inode, leaving the name in place. */
static int
dir_set(struct nufs *v, struct nufs_dinode *dir, const char *name, uint32_t ino)
{
	uint8_t buf[NUFS_DIRBLKSIZ];
	long long off;
	int len = (int)strlen(name);

	for (off = 0; off < (long long)dir->size; off += v->dirblksiz) {
		int o = 0;

		if (read_dirblk(v, dir, off, buf) != 0)
			return -1;
		while (o < v->dirblksiz) {
			uint32_t eino = nufs_get32(v, buf, o + DIR_INO);
			int reclen = nufs_get16(v, buf, o + DIR_RECLEN);
			int elen = nufs_get16(v, buf, o + DIR_NAMLEN);

			if (reclen <= 0 || o + reclen > v->dirblksiz) {
				nufs_err(v, "corrupt directory block at %lld",
				    off);
				return -1;
			}
			if (eino != 0 && elen == len &&
			    memcmp(buf + o + DIR_NAME, name, (size_t)elen) == 0) {
				nufs_put32(v, buf, o + DIR_INO, ino);
				return write_dirblk(v, dir, off, buf);
			}
			o += reclen;
		}
	}
	nufs_errc(v, ENOENT, "no entry \"%s\"", name);
	return -1;
}

/*
 * Drop one reference to an inode whose directory entry is already gone.
 * A directory always goes away entirely: its own "." held the second link.
 */
static int
drop_inode(struct nufs *v, struct nufs_dinode *node, int isdir)
{
	if (!isdir && --node->nlink > 0) {
		node->ctime = (uint32_t)time(NULL);
		return nufs_inode_write(v, node);
	}
	if (nufs_truncate(v, node, 0) != 0)
		return -1;
	node->mode = 0;
	node->nlink = 0;
	if (nufs_inode_write(v, node) != 0)
		return -1;
	return nufs_free_inode(v, node->ino, isdir);
}

struct emptyctx { int n; };

static int
empty_cb(void *arg, uint32_t ino, const char *name, int namlen)
{
	struct emptyctx *c = arg;

	(void)ino;
	(void)namlen;
	if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0)
		c->n++;
	return 0;
}

/* --- inode creation ----------------------------------------------------- */

static void
init_inode(struct nufs *v, struct nufs_dinode *dp, uint32_t ino, uint16_t mode,
    uint16_t uid, uint16_t gid)
{
	uint32_t now = (uint32_t)time(NULL);

	(void)v;
	memset(dp, 0, sizeof(*dp));
	dp->ino = ino;
	dp->mode = mode;
	dp->nlink = 1;
	dp->uid = uid;
	dp->gid = gid;
	dp->atime = dp->mtime = dp->ctime = now;
	dp->gen = now;
}

/* Split "/a/b/c" into the inode of "/a/b" and the name "c". */
int
nufs_lookup_parent(struct nufs *v, const char *path, struct nufs_dinode *parent,
    char *name, size_t namesz)
{
	char dir[1024];
	const char *base;
	size_t n;

	base = strrchr(path, '/');
	if (base == NULL) {
		base = path;
		snprintf(dir, sizeof(dir), "/");
	} else {
		n = (size_t)(base - path);
		if (n == 0)
			snprintf(dir, sizeof(dir), "/");
		else {
			if (n >= sizeof(dir))
				n = sizeof(dir) - 1;
			memcpy(dir, path, n);
			dir[n] = '\0';
		}
		base++;
	}
	if (*base == '\0') {
		nufs_errc(v, EINVAL, "\"%s\" has no final component", path);
		return -1;
	}
	snprintf(name, namesz, "%s", base);
	if (nufs_lookup(v, dir, parent) != 0)
		return -1;
	if ((parent->mode & S_IFMT_) != S_DIR_) {
		nufs_errc(v, ENOTDIR, "\"%s\" is not a directory", dir);
		return -1;
	}
	return 0;
}

/*
 * Create a file, device, fifo or socket. A device keeps its dev_t in db[0],
 * where NeXT's di_rdev lives (docs/next-headers/ufs_inode.h).
 */
int
nufs_mknod(struct nufs *v, const char *path, uint16_t mode, uint16_t uid,
    uint16_t gid, uint32_t rdev, struct nufs_dinode *out)
{
	struct nufs_dinode parent, node;
	char name[NUFS_MAXNAMLEN + 1];
	uint32_t ino;

	if ((mode & S_IFMT_) == S_DIR_) {
		nufs_errc(v, EINVAL, "use mkdir to create a directory");
		return -1;
	}
	if ((mode & S_IFMT_) == 0)
		mode = (uint16_t)(mode | S_REG_);
	if (nufs_lookup_parent(v, path, &parent, name, sizeof(name)) != 0)
		return -1;
	ino = nufs_alloc_inode(v, (int)(parent.ino / (uint32_t)v->ipg), 0);
	if (ino == 0)
		return -1;
	init_inode(v, &node, ino, mode, uid, gid);
	if ((mode & S_IFMT_) == S_CHR_ || (mode & S_IFMT_) == S_BLK_)
		node.db[0] = (int32_t)rdev;
	if (nufs_inode_write(v, &node) != 0)
		return -1;
	if (nufs_dir_add(v, &parent, name, ino) != 0)
		return -1;
	if (out != NULL)
		*out = node;
	return 0;
}

int
nufs_create(struct nufs *v, const char *path, uint16_t mode, uint16_t uid,
    uint16_t gid, struct nufs_dinode *out)
{
	return nufs_mknod(v, path, (uint16_t)(S_REG_ | (mode & 07777)), uid,
	    gid, 0, out);
}

int
nufs_link(struct nufs *v, const char *existing, const char *newpath)
{
	struct nufs_dinode node, parent;
	char name[NUFS_MAXNAMLEN + 1];

	if (nufs_lookup_nofollow(v, existing, &node) != 0)
		return -1;
	if ((node.mode & S_IFMT_) == S_DIR_) {
		nufs_errc(v, EPERM, "cannot hard link a directory");
		return -1;
	}
	if (nufs_lookup_parent(v, newpath, &parent, name, sizeof(name)) != 0)
		return -1;
	if (nufs_dir_add(v, &parent, name, node.ino) != 0)
		return -1;
	node.nlink++;
	node.ctime = (uint32_t)time(NULL);
	return nufs_inode_write(v, &node);
}

/* NULL leaves that timestamp alone. */
int
nufs_utimes(struct nufs *v, const char *path, const uint32_t *atime,
    const uint32_t *mtime)
{
	struct nufs_dinode d;

	if (nufs_lookup_nofollow(v, path, &d) != 0)
		return -1;
	if (atime != NULL)
		d.atime = *atime;
	if (mtime != NULL)
		d.mtime = *mtime;
	d.ctime = (uint32_t)time(NULL);
	return nufs_inode_write(v, &d);
}

int
nufs_mkdir(struct nufs *v, const char *path, uint16_t mode, uint16_t uid,
    uint16_t gid)
{
	struct nufs_dinode parent, node;
	char name[NUFS_MAXNAMLEN + 1];
	uint8_t buf[NUFS_DIRBLKSIZ];
	uint32_t ino;
	int dot = dirsiz(1);

	if (nufs_lookup_parent(v, path, &parent, name, sizeof(name)) != 0)
		return -1;
	ino = nufs_alloc_inode(v, (int)(parent.ino / (uint32_t)v->ipg), 1);
	if (ino == 0)
		return -1;
	init_inode(v, &node, ino, (uint16_t)(S_DIR_ | (mode & 07777)), uid, gid);
	node.nlink = 2;				/* the entry and "." */
	if (nufs_inode_write(v, &node) != 0)
		return -1;

	memset(buf, 0, sizeof(buf));
	nufs_put32(v, buf, DIR_INO, ino);
	nufs_put16(v, buf, DIR_RECLEN, (uint16_t)dot);
	nufs_put16(v, buf, DIR_NAMLEN, 1);
	buf[DIR_NAME] = '.';
	nufs_put32(v, buf, dot + DIR_INO, parent.ino);
	nufs_put16(v, buf, dot + DIR_RECLEN, (uint16_t)(v->dirblksiz - dot));
	nufs_put16(v, buf, dot + DIR_NAMLEN, 2);
	buf[dot + DIR_NAME] = '.';
	buf[dot + DIR_NAME + 1] = '.';
	if (nufs_file_write(v, &node, buf, 0, v->dirblksiz) != v->dirblksiz)
		return -1;

	if (nufs_dir_add(v, &parent, name, ino) != 0)
		return -1;
	parent.nlink++;				/* the child's ".." */
	parent.ctime = parent.mtime = (uint32_t)time(NULL);
	return nufs_inode_write(v, &parent);
}

int
nufs_symlink(struct nufs *v, const char *target, const char *path,
    uint16_t uid, uint16_t gid)
{
	struct nufs_dinode parent, node;
	char name[NUFS_MAXNAMLEN + 1];
	size_t len = strlen(target);
	uint32_t ino;

	if (nufs_lookup_parent(v, path, &parent, name, sizeof(name)) != 0)
		return -1;
	ino = nufs_alloc_inode(v, (int)(parent.ino / (uint32_t)v->ipg), 0);
	if (ino == 0)
		return -1;
	init_inode(v, &node, ino, (uint16_t)(S_LNK_ | 0777), uid, gid);
	if (len <= NUFS_MAXFASTLINK && !v->aux) {	/* A/UX has no fastlink */
		node.flags |= NUFS_IC_FASTLINK;
		memcpy(node.symlink, target, len);
		node.size = len;
		if (nufs_inode_write(v, &node) != 0)
			return -1;
	} else {
		if (nufs_inode_write(v, &node) != 0)
			return -1;
		if (nufs_file_write(v, &node, target, 0, (long)len) != (long)len)
			return -1;
	}
	return nufs_dir_add(v, &parent, name, ino);
}

int
nufs_unlink(struct nufs *v, const char *path)
{
	struct nufs_dinode parent, node;
	char name[NUFS_MAXNAMLEN + 1];
	uint32_t ino;

	if (nufs_lookup_parent(v, path, &parent, name, sizeof(name)) != 0)
		return -1;
	if (nufs_dir_lookup(v, &parent, name, &ino) != 0)
		return -1;
	if (nufs_inode_read(v, ino, &node) != 0)
		return -1;
	if ((node.mode & S_IFMT_) == S_DIR_) {
		nufs_errc(v, EISDIR, "\"%s\" is a directory", path);
		return -1;
	}
	if (nufs_dir_remove(v, &parent, name) != 0)
		return -1;
	return drop_inode(v, &node, 0);
}

int
nufs_rmdir(struct nufs *v, const char *path)
{
	struct nufs_dinode parent, node;
	char name[NUFS_MAXNAMLEN + 1];
	struct emptyctx c = { 0 };
	uint32_t ino;

	if (nufs_lookup_parent(v, path, &parent, name, sizeof(name)) != 0)
		return -1;
	if (nufs_dir_lookup(v, &parent, name, &ino) != 0)
		return -1;
	if (ino == NUFS_ROOTINO) {
		nufs_errc(v, EBUSY, "cannot remove the root directory");
		return -1;
	}
	if (nufs_inode_read(v, ino, &node) != 0)
		return -1;
	if ((node.mode & S_IFMT_) != S_DIR_) {
		nufs_errc(v, ENOTDIR, "\"%s\" is not a directory", path);
		return -1;
	}
	if (nufs_readdir(v, &node, empty_cb, &c) != 0)
		return -1;
	if (c.n != 0) {
		nufs_errc(v, ENOTEMPTY, "\"%s\" is not empty", path);
		return -1;
	}
	if (nufs_dir_remove(v, &parent, name) != 0)
		return -1;
	if (drop_inode(v, &node, 1) != 0)
		return -1;
	if (nufs_inode_read(v, parent.ino, &parent) != 0)
		return -1;
	parent.nlink--;
	parent.ctime = parent.mtime = (uint32_t)time(NULL);
	return nufs_inode_write(v, &parent);
}

/* Is `ino` an ancestor of the directory `dir` (or the directory itself)? */
static int
is_ancestor(struct nufs *v, uint32_t ino, uint32_t dir, int *yes)
{
	struct nufs_dinode d;
	uint32_t cur = dir;
	int depth;

	*yes = 0;
	for (depth = 0; depth < 256; depth++) {
		if (cur == ino) {
			*yes = 1;
			return 0;
		}
		if (cur == NUFS_ROOTINO)
			return 0;
		if (nufs_inode_read(v, cur, &d) != 0)
			return -1;
		if (nufs_dir_lookup(v, &d, "..", &cur) != 0)
			return -1;
	}
	nufs_errc(v, ELOOP, "directory tree is too deep");
	return -1;
}

int
nufs_rename(struct nufs *v, const char *from, const char *to, int flags)
{
	struct nufs_dinode fromdir, todir, node, old;
	char fname[NUFS_MAXNAMLEN + 1], tname[NUFS_MAXNAMLEN + 1];
	uint32_t ino, victim = 0;
	int isdir, hasvictim, victimisdir = 0, moved;

	if (nufs_lookup_parent(v, from, &fromdir, fname, sizeof(fname)) != 0)
		return -1;
	if (nufs_dir_lookup(v, &fromdir, fname, &ino) != 0)
		return -1;
	if (nufs_inode_read(v, ino, &node) != 0)
		return -1;
	isdir = (node.mode & S_IFMT_) == S_DIR_;
	if (ino == NUFS_ROOTINO) {
		nufs_errc(v, EBUSY, "cannot rename the root directory");
		return -1;
	}

	if (nufs_lookup_parent(v, to, &todir, tname, sizeof(tname)) != 0)
		return -1;
	hasvictim = nufs_dir_lookup(v, &todir, tname, &victim) == 0;
	if (hasvictim && victim == ino)
		return 0;			/* the same name, or a hard link */
	moved = todir.ino != fromdir.ino;

	if (isdir) {
		int inside;

		if (is_ancestor(v, ino, todir.ino, &inside) != 0)
			return -1;
		if (inside) {
			nufs_errc(v, EINVAL, "cannot move a directory into "
			    "itself");
			return -1;
		}
	}

	if (hasvictim) {
		if ((flags & NUFS_RENAME_NOREPLACE) != 0) {
			nufs_errc(v, EEXIST, "\"%s\" already exists", to);
			return -1;
		}
		if (nufs_inode_read(v, victim, &old) != 0)
			return -1;
		victimisdir = (old.mode & S_IFMT_) == S_DIR_;
		if (isdir && !victimisdir) {
			nufs_errc(v, ENOTDIR, "\"%s\" is not a directory", to);
			return -1;
		}
		if (!isdir && victimisdir) {
			nufs_errc(v, EISDIR, "\"%s\" is a directory", to);
			return -1;
		}
		if (victimisdir) {
			struct emptyctx c = { 0 };

			if (victim == NUFS_ROOTINO) {
				nufs_errc(v, EBUSY, "cannot replace the root "
				    "directory");
				return -1;
			}
			if (nufs_readdir(v, &old, empty_cb, &c) != 0)
				return -1;
			if (c.n != 0) {
				nufs_errc(v, ENOTEMPTY, "\"%s\" is not empty",
				    to);
				return -1;
			}
		}
	}

	if (isdir && moved) {
		/* The directory's ".." and both parents' link counts move. */
		uint8_t buf[NUFS_DIRBLKSIZ];
		int dot = dirsiz(1);

		if (nufs_file_read(v, &node, buf, 0, v->dirblksiz) !=
		    v->dirblksiz)
			return -1;
		nufs_put32(v, buf, dot + DIR_INO, todir.ino);
		if (nufs_file_write(v, &node, buf, 0, v->dirblksiz) !=
		    v->dirblksiz)
			return -1;
	}

	if (hasvictim) {
		if (dir_set(v, &todir, tname, ino) != 0)
			return -1;
		if (drop_inode(v, &old, victimisdir) != 0)
			return -1;
		if (victimisdir) {
			/* The victim's ".." no longer counts against todir. */
			if (nufs_inode_read(v, todir.ino, &todir) != 0)
				return -1;
			todir.nlink--;
			todir.ctime = (uint32_t)time(NULL);
			if (nufs_inode_write(v, &todir) != 0)
				return -1;
		}
	} else if (nufs_dir_add(v, &todir, tname, ino) != 0)
		return -1;

	/* Re-read: touching the target may have changed the source parent. */
	if (nufs_inode_read(v, fromdir.ino, &fromdir) != 0)
		return -1;
	if (nufs_dir_remove(v, &fromdir, fname) != 0)
		return -1;
	if (isdir && moved) {
		if (nufs_inode_read(v, todir.ino, &todir) != 0)
			return -1;
		todir.nlink++;
		todir.ctime = (uint32_t)time(NULL);
		if (nufs_inode_write(v, &todir) != 0)
			return -1;
		if (nufs_inode_read(v, fromdir.ino, &fromdir) != 0)
			return -1;
		fromdir.nlink--;
		fromdir.ctime = (uint32_t)time(NULL);
		if (nufs_inode_write(v, &fromdir) != 0)
			return -1;
	}
	if (nufs_inode_read(v, ino, &node) != 0)
		return -1;
	node.ctime = (uint32_t)time(NULL);
	return nufs_inode_write(v, &node);
}

int
nufs_chmod(struct nufs *v, const char *path, uint16_t mode)
{
	struct nufs_dinode d;

	if (nufs_lookup_nofollow(v, path, &d) != 0)
		return -1;
	d.mode = (uint16_t)((d.mode & ~07777) | (mode & 07777));
	d.ctime = (uint32_t)time(NULL);
	return nufs_inode_write(v, &d);
}

int
nufs_chown(struct nufs *v, const char *path, uint16_t uid, uint16_t gid)
{
	struct nufs_dinode d;

	if (nufs_lookup_nofollow(v, path, &d) != 0)
		return -1;
	d.uid = uid;
	d.gid = gid;
	d.ctime = (uint32_t)time(NULL);
	return nufs_inode_write(v, &d);
}
