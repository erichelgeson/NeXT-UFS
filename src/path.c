#include "nextufs.h"
#include <string.h>
#include <stdio.h>

#define MAXSYMLINKS	8
#define PATHMAX		1024

/* Collapse "." and ".." textually so a symlink target can be re-resolved. */
static void
normalize(char *p)
{
	char tmp[PATHMAX], *seg, *save = NULL, *parts[128];
	int np = 0, i;
	size_t n = 0;

	snprintf(tmp, sizeof(tmp), "%s", p);
	for (seg = strtok_r(tmp, "/", &save); seg != NULL;
	    seg = strtok_r(NULL, "/", &save)) {
		if (strcmp(seg, ".") == 0)
			continue;
		if (strcmp(seg, "..") == 0) {
			if (np > 0)
				np--;
			continue;
		}
		if (np < (int)(sizeof(parts) / sizeof(parts[0])))
			parts[np++] = seg;
	}
	p[0] = '\0';
	for (i = 0; i < np; i++)
		n += (size_t)snprintf(p + n, PATHMAX - n, "/%s", parts[i]);
	if (n == 0)
		strcpy(p, "/");
}

static int
lookup(struct nufs *v, const char *path, struct nufs_dinode *dp, int depth)
{
	char work[PATHMAX], cur[PATHMAX], link[PATHMAX];
	char next[3 * PATHMAX];
	char *seg, *save = NULL;

	if (depth > MAXSYMLINKS) {
		nufs_err(v, "too many levels of symbolic links");
		return -1;
	}
	if (nufs_inode_read(v, NUFS_ROOTINO, dp) != 0)
		return -1;
	snprintf(work, sizeof(work), "%s", path);
	normalize(work);
	strcpy(cur, "");			/* path of dp, without a tail / */

	for (seg = strtok_r(work, "/", &save); seg != NULL;
	    seg = strtok_r(NULL, "/", &save)) {
		uint32_t ino;

		if (nufs_dir_lookup(v, dp, seg, &ino) != 0)
			return -1;
		if (nufs_inode_read(v, ino, dp) != 0)
			return -1;

		if ((dp->mode & 0170000) == 0120000) {
			const char *rest = (save != NULL && *save != '\0') ?
			    save : "";

			if (nufs_readlink(v, dp, link, sizeof(link)) != 0)
				return -1;
			if (link[0] == '/')
				snprintf(next, sizeof(next), "%s/%s", link, rest);
			else
				snprintf(next, sizeof(next), "%s/%s/%s", cur,
				    link, rest);
			if (strlen(next) >= PATHMAX) {
				nufs_err(v, "resolved path is too long");
				return -1;
			}
			return lookup(v, next, dp, depth + 1);
		}
		snprintf(cur + strlen(cur), sizeof(cur) - strlen(cur), "/%s", seg);
	}
	return 0;
}

int
nufs_lookup(struct nufs *v, const char *path, struct nufs_dinode *dp)
{
	return lookup(v, path, dp, 0);
}

/* Like nufs_lookup, but the final component is not followed if it is a link. */
int
nufs_lookup_nofollow(struct nufs *v, const char *path, struct nufs_dinode *dp)
{
	struct nufs_dinode parent;
	char name[NUFS_MAXNAMLEN + 1], work[PATHMAX];
	uint32_t ino;

	snprintf(work, sizeof(work), "%s", path);
	normalize(work);
	if (strcmp(work, "/") == 0)
		return nufs_inode_read(v, NUFS_ROOTINO, dp);
	if (nufs_lookup_parent(v, work, &parent, name, sizeof(name)) != 0)
		return -1;
	if (nufs_dir_lookup(v, &parent, name, &ino) != 0)
		return -1;
	return nufs_inode_read(v, ino, dp);
}
