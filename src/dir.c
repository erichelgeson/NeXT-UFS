#include "nextufs.h"
#include <stdlib.h>
#include <string.h>

int
nufs_readdir(struct nufs *v, const struct nufs_dinode *dp, nufs_dir_cb cb,
    void *arg)
{
	uint8_t *buf;
	long long off;
	int rc = 0;

	if ((dp->mode & 0170000) != 0040000) {
		nufs_err(v, "inode %u is not a directory", dp->ino);
		return -1;
	}
	buf = malloc(NUFS_DIRBLKSIZ);
	if (buf == NULL) {
		nufs_err(v, "out of memory");
		return -1;
	}
	for (off = 0; off < (long long)dp->size; off += NUFS_DIRBLKSIZ) {
		int o = 0;
		long got = nufs_file_read(v, dp, buf, off, NUFS_DIRBLKSIZ);

		if (got < 0) {
			rc = -1;
			break;
		}
		while (o + 8 <= got) {
			uint32_t ino = nufs_get32(v, buf, o + DIR_INO);
			int reclen = nufs_get16(v, buf, o + DIR_RECLEN);
			int namlen = nufs_get16(v, buf, o + DIR_NAMLEN);
			char name[NUFS_MAXNAMLEN + 1];

			if (reclen <= 0 || o + reclen > got ||
			    namlen > NUFS_MAXNAMLEN || 8 + namlen > reclen) {
				nufs_err(v, "corrupt directory entry in inode %u"
				    " at offset %lld", dp->ino, off + o);
				rc = -1;
				break;
			}
			if (ino != 0) {
				memcpy(name, buf + o + DIR_NAME, (size_t)namlen);
				name[namlen] = '\0';
				rc = cb(arg, ino, name, namlen);
				if (rc != 0)
					goto out;
			}
			o += reclen;
		}
		if (rc != 0)
			break;
	}
out:
	free(buf);
	return rc < 0 ? -1 : 0;
}

struct lookup_ctx {
	const char	*name;
	uint32_t	ino;
};

static int
lookup_cb(void *arg, uint32_t ino, const char *name, int namlen)
{
	struct lookup_ctx *c = arg;

	(void)namlen;
	if (strcmp(name, c->name) == 0) {
		c->ino = ino;
		return 1;			/* stop the walk */
	}
	return 0;
}

int
nufs_dir_lookup(struct nufs *v, const struct nufs_dinode *dir, const char *name,
    uint32_t *ino)
{
	struct lookup_ctx c;

	c.name = name;
	c.ino = 0;
	if (nufs_readdir(v, dir, lookup_cb, &c) < 0)
		return -1;
	if (c.ino == 0) {
		nufs_err(v, "no entry \"%s\"", name);
		return -1;
	}
	*ino = c.ino;
	return 0;
}
