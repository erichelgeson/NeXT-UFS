#include "nextufs.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static const char *progname = "nextufs";

static void
usage(void)
{
	fprintf(stderr,
"usage: %s [-p part] <image> <command> [args]\n"
"\n"
"  -p is a NeXT partition letter (a..h), or on an A/UX disk a partition\n"
"  number or name from the Apple Partition Map.\n"
"\n"
"  info                    show the disk label and filesystem geometry\n"
"  ls [-l] [path]          list a directory\n"
"  cat <path>              write a file to stdout\n"
"  get <path> [dest]       copy a file out of the image\n"
"  find [path]             list every path under a directory\n"
"  readlink <path>         show a symlink target\n"
"  stat <path>             show one inode\n"
"\n"
"  put [-r] <host> <path>  copy a file or directory tree into the image\n"
"  get [-r] <path> <host>  copy a file or directory tree out\n"
"  rm <path>               remove a file\n"
"  mkdir <path>            create a directory\n"
"  rmdir <path>            remove an empty directory\n"
"  ln -s <target> <path>   create a symbolic link\n"
"  ln <existing> <path>    create a hard link\n"
"  truncate <path> <size>  set a file's length, padding with zeros\n"
"  putat <host> <path> <off>  write a host file into an existing file at <off>\n"
"  mv <from> <to>          rename or move\n"
"  chmod <mode> <path>     change permissions (octal)\n"
"  chown <uid>[:<gid>] <p> change owner\n"
"\n"
"  fsck [-y]               check the filesystem, -y repairs it\n"
"  mkfs <MB> [label]       create a labelled, empty NeXT volume\n",
	    progname);
	exit(2);
}

static struct nufs *
openvol(const char *img, const char *part, int rw)
{
	char err[256];
	struct nufs *v = nufs_open(img, part, rw, err, sizeof(err));

	if (v == NULL) {
		fprintf(stderr, "%s: %s: %s\n", progname, img, err);
		exit(1);
	}
	return v;
}

static void
die(struct nufs *v, const char *what)
{
	fprintf(stderr, "%s: %s: %s\n", progname, what, v->err);
	/*
	 * Close rather than exit outright. A command that fails partway has
	 * already put the volume back in a consistent state in memory, and
	 * throwing that away unwritten is what leaves an inode allocated with
	 * nothing pointing at it.
	 */
	nufs_close(v);
	exit(1);
}

static const char *
modestr(uint16_t mode)
{
	static char s[11];
	static const char *rwx[] = { "---", "--x", "-w-", "-wx", "r--", "r-x",
	    "rw-", "rwx" };

	switch (mode & 0170000) {
	case 0040000: s[0] = 'd'; break;
	case 0120000: s[0] = 'l'; break;
	case 0100000: s[0] = '-'; break;
	case 0020000: s[0] = 'c'; break;
	case 0060000: s[0] = 'b'; break;
	case 0010000: s[0] = 'p'; break;
	case 0140000: s[0] = 's'; break;
	default: s[0] = '?'; break;
	}
	memcpy(s + 1, rwx[(mode >> 6) & 7], 3);
	memcpy(s + 4, rwx[(mode >> 3) & 7], 3);
	memcpy(s + 7, rwx[mode & 7], 3);
	s[10] = '\0';
	if (mode & 04000) s[3] = (mode & 0100) ? 's' : 'S';
	if (mode & 02000) s[6] = (mode & 0010) ? 's' : 'S';
	if (mode & 01000) s[9] = (mode & 0001) ? 't' : 'T';
	return s;
}

struct lsctx {
	struct nufs	*v;
	int		lng;
};

static int
ls_cb(void *arg, uint32_t ino, const char *name, int namlen)
{
	struct lsctx *c = arg;
	struct nufs_dinode d;
	char when[32];
	struct tm tm;
	time_t t;

	(void)namlen;
	if (!c->lng) {
		printf("%s\n", name);
		return 0;
	}
	if (nufs_inode_read(c->v, ino, &d) != 0) {
		printf("?????????  %7u  %s\n", ino, name);
		return 0;
	}
	t = (time_t)d.mtime;
	gmtime_r(&t, &tm);
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
	printf("%s %3d %5u %5u %10llu %s %7u %s", modestr(d.mode), d.nlink,
	    d.uid, d.gid, (unsigned long long)d.size, when, ino, name);
	if ((d.mode & 0170000) == 0120000) {
		char link[1024];

		if (nufs_readlink(c->v, &d, link, sizeof(link)) == 0)
			printf(" -> %s", link);
	}
	putchar('\n');
	return 0;
}

struct findctx {
	struct nufs	*v;
	char		path[1024];
	int		depth;
};

static int find_cb(void *arg, uint32_t ino, const char *name, int namlen);

static int
find_walk(struct findctx *c, uint32_t ino)
{
	struct nufs_dinode d;

	if (nufs_inode_read(c->v, ino, &d) != 0)
		return -1;
	if ((d.mode & 0170000) != 0040000)
		return 0;
	if (c->depth > 64)
		return 0;
	return nufs_readdir(c->v, &d, find_cb, c);
}

static int
find_cb(void *arg, uint32_t ino, const char *name, int namlen)
{
	struct findctx *c = arg;
	size_t n = strlen(c->path);

	(void)namlen;
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return 0;
	snprintf(c->path + n, sizeof(c->path) - n, "/%s", name);
	printf("%s\n", c->path);
	c->depth++;
	find_walk(c, ino);
	c->depth--;
	c->path[n] = '\0';
	return 0;
}

static void
cmd_info(struct nufs *v)
{
	if (v->label.valid || v->label.version[0] != '\0')
		nufs_label_print(&v->label, stdout);
	if (v->apm.valid)
		nufs_apm_print(&v->apm, stdout);
	if (v->partno >= 0)
		printf("using     partition %c at byte offset 0x%llx\n",
		    'a' + v->partno, (unsigned long long)v->partoff);
	else if (v->apmno >= 0)
		printf("using     partition %d \"%s\" at byte offset 0x%llx\n",
		    v->apmno + 1, v->apm.part[v->apmno].name,
		    (unsigned long long)v->partoff);
	else
		printf("using     filesystem at byte offset 0x%llx (no label)\n",
		    (unsigned long long)v->partoff);
	printf("variant   %s\n", v->solaris ? "Solaris 2 (SVR4 UFS),"
	    " readable but not writable" :
	    v->dyncg ? "SunOS or a later BSD"
	    " (dynamic cylinder groups)" :
	    v->aux ? "A/UX (Macintosh metadata in the spare inode fields)" :
	    "NeXT");
	printf("byteorder %s\n", v->be ? "big-endian" : "little-endian");
	printf("geometry  %d frags of %d bytes, block %d, %d frags/block\n",
	    v->size, v->fsize, v->bsize, v->frag);
	printf("cyl grps  %d of %d cylinders, %d inodes and %d frags each\n",
	    v->ncg, v->cpg, v->ipg, v->fpg);
	printf("layout    sblkno %d cblkno %d iblkno %d dblkno %d cgoffset %d"
	    " cgmask 0x%08x\n", v->sblkno, v->cblkno, v->iblkno, v->dblkno,
	    v->cgoffset, (unsigned)v->cgmask);
	printf("summary   csaddr %d cssize %d cgsize %d\n", v->csaddr,
	    v->cssize, v->cgsize);
	printf("free      %d dirs, %d blocks, %d inodes, %d frags\n",
	    (int32_t)nufs_get32(v, v->sb, FS_CSTOTAL),
	    (int32_t)nufs_get32(v, v->sb, FS_CSTOTAL + 4),
	    (int32_t)nufs_get32(v, v->sb, FS_CSTOTAL + 8),
	    (int32_t)nufs_get32(v, v->sb, FS_CSTOTAL + 12));
	if (v->aux)			/* A/UX records nothing either way */
		printf("state     last written %u\n",
		    nufs_get32(v, v->sb, FS_TIME));
	else
		printf("state     fmod %d %s %d (%s), last written %u\n",
		    v->sb[FS_FMOD], v->dyncg ? "fs_clean" : "fs_state",
		    v->sb[FS_STATE], nufs_is_clean(v) ? "clean" : "not clean",
		    nufs_get32(v, v->sb, FS_TIME));
	printf("mounted   %s\n", (const char *)v->sb + FS_FSMNT);
}

static void
copy_out(struct nufs *v, const struct nufs_dinode *d, FILE *out)
{
	char buf[65536];
	long long off = 0;

	while (off < (long long)d->size) {
		long n = nufs_file_read(v, d, buf, off, (long)sizeof(buf));

		if (n < 0)
			die(v, "read");
		if (n == 0)
			break;
		if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
			fprintf(stderr, "%s: write failed: %s\n", progname,
			    strerror(errno));
			exit(1);
		}
		off += n;
	}
}


/* Copy one host file into the image, replacing whatever is there. */
static int
put_file(struct nufs *v, const char *host, const char *path, mode_t mode)
{
	struct nufs_dinode node;
	char buf[65536];
	FILE *in;
	size_t got;
	long long off = 0;

	in = fopen(host, "rb");
	if (in == NULL) {
		fprintf(stderr, "%s: %s: %s\n", progname, host,
		    strerror(errno));
		return -1;
	}
	if (nufs_lookup_nofollow(v, path, &node) == 0 &&
	    nufs_unlink(v, path) != 0) {
		fclose(in);
		return -1;
	}
	if (nufs_create(v, path, (uint16_t)(mode & 07777), 0, 0, &node) != 0) {
		fclose(in);
		return -1;
	}
	while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (nufs_file_write(v, &node, buf, off, (long)got) !=
		    (long)got) {
			/* Half a file is worse than none: take it back out. */
			char saved[256];

			snprintf(saved, sizeof(saved), "%s", v->err);
			nufs_unlink(v, path);
			snprintf(v->err, sizeof(v->err), "%s", saved);
			fclose(in);
			return -1;
		}
		off += (long long)got;
	}
	fclose(in);
	return 0;
}

static int
put_tree(struct nufs *v, const char *host, const char *path)
{
	struct nufs_dinode d;
	struct dirent *de;
	struct stat st;
	DIR *dir;
	char h[2048], p[1024];

	if (lstat(host, &st) != 0) {
		fprintf(stderr, "%s: %s: %s\n", progname, host,
		    strerror(errno));
		return -1;
	}
	if (S_ISLNK(st.st_mode)) {
		char target[1024];
		ssize_t n = readlink(host, target, sizeof(target) - 1);

		if (n < 0)
			return -1;
		target[n] = '\0';
		return nufs_symlink(v, target, path, 0, 0);
	}
	if (S_ISREG(st.st_mode))
		return put_file(v, host, path, st.st_mode);
	if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "%s: %s: not a file, directory or symlink\n",
		    progname, host);
		return -1;
	}
	if (nufs_lookup_nofollow(v, path, &d) != 0 &&
	    nufs_mkdir(v, path, (uint16_t)(st.st_mode & 07777), 0, 0) != 0)
		return -1;
	dir = opendir(host);
	if (dir == NULL) {
		fprintf(stderr, "%s: %s: %s\n", progname, host,
		    strerror(errno));
		return -1;
	}
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(h, sizeof(h), "%s/%s", host, de->d_name);
		snprintf(p, sizeof(p), "%s/%s", path, de->d_name);
		if (put_tree(v, h, p) != 0) {
			closedir(dir);
			return -1;
		}
	}
	closedir(dir);
	return 0;
}

struct getctx {
	struct nufs	*v;
	char		path[1024];
	char		host[2048];
};

static int get_cb(void *arg, uint32_t ino, const char *name, int namlen);

static int
get_tree(struct getctx *c, const struct nufs_dinode *d)
{
	if ((d->mode & 0170000) == 0040000) {
		if (mkdir(c->host, d->mode & 07777) != 0 && errno != EEXIST) {
			fprintf(stderr, "%s: %s: %s\n", progname, c->host,
			    strerror(errno));
			return -1;
		}
		return nufs_readdir(c->v, d, get_cb, c);
	}
	if ((d->mode & 0170000) == 0120000) {
		char link[1024];

		if (nufs_readlink(c->v, d, link, sizeof(link)) != 0)
			return -1;
		unlink(c->host);
		if (symlink(link, c->host) != 0) {
			fprintf(stderr, "%s: %s: %s\n", progname, c->host,
			    strerror(errno));
			return -1;
		}
		return 0;
	}
	if ((d->mode & 0170000) == 0100000) {
		FILE *out = fopen(c->host, "wb");

		if (out == NULL) {
			fprintf(stderr, "%s: %s: %s\n", progname, c->host,
			    strerror(errno));
			return -1;
		}
		copy_out(c->v, d, out);
		fclose(out);
		(void)chmod(c->host, d->mode & 07777);	/* best effort */
		return 0;
	}
	return 0;				/* devices and sockets: skip */
}

static int
get_cb(void *arg, uint32_t ino, const char *name, int namlen)
{
	struct getctx *c = arg;
	size_t np = strlen(c->path), nh = strlen(c->host);
	struct nufs_dinode d;
	int rc;

	(void)namlen;
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return 0;
	if (nufs_inode_read(c->v, ino, &d) != 0)
		return -1;
	snprintf(c->path + np, sizeof(c->path) - np, "/%s", name);
	snprintf(c->host + nh, sizeof(c->host) - nh, "/%s", name);
	rc = get_tree(c, &d);
	c->path[np] = '\0';
	c->host[nh] = '\0';
	return rc;
}

int
main(int argc, char **argv)
{
	const char *part = NULL, *img, *cmd;
	struct nufs *v;
	struct nufs_dinode d;
	int i = 1;

	if (argc > 0)
		progname = argv[0];
	while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
		if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
			part = argv[++i];
		else
			usage();
		i++;
	}
	if (argc - i < 2)
		usage();
	img = argv[i++];
	cmd = argv[i++];

	if (strcmp(cmd, "info") == 0) {
		v = openvol(img, part, 0);
		cmd_info(v);
	} else if (strcmp(cmd, "ls") == 0) {
		struct lsctx c;
		const char *path = "/";

		if (i < argc && strcmp(argv[i], "-l") == 0) {
			i++;
		}
		v = openvol(img, part, 0);
		c.v = v;
		c.lng = 1;
		if (i < argc)
			path = argv[i];
		if (nufs_lookup(v, path, &d) != 0)
			die(v, path);
		if ((d.mode & 0170000) != 0040000)
			ls_cb(&c, d.ino, path, (int)strlen(path));
		else if (nufs_readdir(v, &d, ls_cb, &c) != 0)
			die(v, path);
	} else if (strcmp(cmd, "cat") == 0 || strcmp(cmd, "get") == 0) {
		FILE *out = stdout;

		if (strcmp(cmd, "get") == 0 && i < argc &&
		    strcmp(argv[i], "-r") == 0) {
			struct getctx c;

			i++;
			if (i + 1 >= argc)
				usage();
			v = openvol(img, part, 0);
			if (nufs_lookup(v, argv[i], &d) != 0)
				die(v, argv[i]);
			c.v = v;
			snprintf(c.path, sizeof(c.path), "%s", argv[i]);
			snprintf(c.host, sizeof(c.host), "%s", argv[i + 1]);
			if (get_tree(&c, &d) != 0)
				die(v, argv[i]);
			nufs_close(v);
			return 0;
		}
		if (i >= argc)
			usage();
		v = openvol(img, part, 0);
		if (nufs_lookup(v, argv[i], &d) != 0)
			die(v, argv[i]);
		if (strcmp(cmd, "get") == 0 && i + 1 < argc) {
			out = fopen(argv[i + 1], "wb");
			if (out == NULL) {
				fprintf(stderr, "%s: %s: %s\n", progname,
				    argv[i + 1], strerror(errno));
				exit(1);
			}
		}
		copy_out(v, &d, out);
		if (out != stdout)
			fclose(out);
	} else if (strcmp(cmd, "find") == 0) {
		struct findctx c;
		const char *path = i < argc ? argv[i] : "/";

		v = openvol(img, part, 0);
		c.v = v;
		c.depth = 0;
		if (nufs_lookup(v, path, &d) != 0)
			die(v, path);
		snprintf(c.path, sizeof(c.path), "%s",
		    strcmp(path, "/") == 0 ? "" : path);
		if (find_walk(&c, d.ino) != 0)
			die(v, path);
	} else if (strcmp(cmd, "readlink") == 0) {
		char link[1024];

		if (i >= argc)
			usage();
		v = openvol(img, part, 0);
		if (nufs_lookup_nofollow(v, argv[i], &d) != 0)
			die(v, argv[i]);
		if ((d.mode & 0170000) != 0120000) {
			fprintf(stderr, "%s: %s: not a symbolic link\n",
			    progname, argv[i]);
			exit(1);
		}
		if (nufs_readlink(v, &d, link, sizeof(link)) != 0)
			die(v, argv[i]);
		printf("%s\n", link);
	} else if (strcmp(cmd, "stat") == 0) {
		if (i >= argc)
			usage();
		v = openvol(img, part, 0);
		if (nufs_lookup_nofollow(v, argv[i], &d) != 0)
			die(v, argv[i]);
		printf("inode   %u\n", d.ino);
		printf("mode    %s (0%o)\n", modestr(d.mode), d.mode);
		printf("links   %d\n", d.nlink);
		printf("owner   %u:%u\n", d.uid, d.gid);
		printf("size    %llu\n", (unsigned long long)d.size);
		if (v->aux) {
			/*
			 * Two of these words mean one thing on a directory and
			 * another on a file; see NeXT-UFS-Spec.md §12.3.
			 */
			if ((d.mode & 0170000) == 0040000) {
				printf("valence %u\n", d.valence);
				printf("mac dir %u\n", d.auxid);
			} else {
				printf("forks   data %u, resource %u\n",
				    d.fdlen, d.valence);
				if (d.auxid != 0 || d.fdcrdat != 0)
					printf("mac date created %u, modified"
					    " %u\n", d.fdcrdat, d.auxid);
				if (d.fdflags != 0)
					printf("fdflags 0x%04x%s\n", d.fdflags,
					    (d.fdflags & NUFS_FDALIAS) ?
					    " (alias)" : "");
			}
			if (d.fdtype[0] != '\0' || d.fdcreator[0] != '\0')
				printf("finder  type \"%s\" creator \"%s\"\n",
				    d.fdtype, d.fdcreator);
		}
		printf("blocks  %u\n", d.blocks);
		printf("flags   0x%x%s\n", d.flags,
		    (d.flags & NUFS_IC_FASTLINK) ? " (fastlink)" : "");
		if (v->dyncg)		/* SunOS keeps a whole timeval */
			printf("times   a %u.%06u m %u.%06u c %u.%06u\n",
			    d.atime, d.ausec, d.mtime, d.musec,
			    d.ctime, d.cusec);
		else
			printf("times   a %u m %u c %u\n", d.atime, d.mtime,
			    d.ctime);
		for (i = 0; i < NUFS_NDADDR; i++)
			if (d.db[i] != 0)
				printf("db[%d]   %d\n", i, d.db[i]);
		for (i = 0; i < NUFS_NIADDR; i++)
			if (d.ib[i] != 0)
				printf("ib[%d]   %d\n", i, d.ib[i]);
	} else if (strcmp(cmd, "mkfs") == 0) {
		long long mb;

		if (i >= argc)
			usage();
		mb = strtoll(argv[i], NULL, 10);
		if (mb <= 0) {
			fprintf(stderr, "%s: bad size \"%s\"\n", progname,
			    argv[i]);
			exit(1);
		}
		return nufs_mkfs(img, mb * 1024, i + 1 < argc ? argv[i + 1] :
		    "NeXT", 0) != 0;
	} else if (strcmp(cmd, "fsck") == 0) {
		int fix = (i < argc && strcmp(argv[i], "-y") == 0);
		int rc;

		v = openvol(img, part, fix);
		rc = nufs_fsck(v, fix);
		nufs_close(v);
		return rc < 0 ? 1 : rc;
	} else if (strcmp(cmd, "put") == 0) {
		int rec = 0;

		if (i < argc && strcmp(argv[i], "-r") == 0) {
			rec = 1;
			i++;
		}
		if (i + 1 >= argc)
			usage();
		v = openvol(img, part, 1);
		if (rec) {
			if (put_tree(v, argv[i], argv[i + 1]) != 0)
				die(v, argv[i + 1]);
		} else {
			struct stat st;

			if (stat(argv[i], &st) != 0) {
				fprintf(stderr, "%s: %s: %s\n", progname,
				    argv[i], strerror(errno));
				exit(1);
			}
			if (put_file(v, argv[i], argv[i + 1], st.st_mode) != 0)
				die(v, argv[i + 1]);
		}
	} else if (strcmp(cmd, "truncate") == 0) {
		if (i + 1 >= argc)
			usage();
		v = openvol(img, part, 1);
		if (nufs_lookup_nofollow(v, argv[i], &d) != 0)
			die(v, argv[i]);
		if (nufs_truncate(v, &d,
		    (uint64_t)strtoull(argv[i + 1], NULL, 10)) != 0)
			die(v, argv[i]);
	} else if (strcmp(cmd, "putat") == 0) {
		char buf[65536];
		long long off;
		FILE *in;
		size_t got;

		if (i + 2 >= argc)
			usage();
		off = strtoll(argv[i + 2], NULL, 10);
		in = fopen(argv[i], "rb");
		if (in == NULL) {
			fprintf(stderr, "%s: %s: %s\n", progname, argv[i],
			    strerror(errno));
			exit(1);
		}
		v = openvol(img, part, 1);
		if (nufs_lookup_nofollow(v, argv[i + 1], &d) != 0 &&
		    nufs_create(v, argv[i + 1], 0644, 0, 0, &d) != 0)
			die(v, argv[i + 1]);
		while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
			if (nufs_file_write(v, &d, buf, off, (long)got) !=
			    (long)got)
				die(v, argv[i + 1]);
			off += (long long)got;
		}
		fclose(in);
	} else if (strcmp(cmd, "mv") == 0) {
		if (i + 1 >= argc)
			usage();
		v = openvol(img, part, 1);
		if (nufs_rename(v, argv[i], argv[i + 1], 0) != 0)
			die(v, argv[i]);
	} else if (strcmp(cmd, "chmod") == 0) {
		if (i + 1 >= argc)
			usage();
		v = openvol(img, part, 1);
		if (nufs_chmod(v, argv[i + 1],
		    (uint16_t)strtol(argv[i], NULL, 8)) != 0)
			die(v, argv[i + 1]);
	} else if (strcmp(cmd, "chown") == 0) {
		char *colon;
		long uid, gid;

		if (i + 1 >= argc)
			usage();
		uid = strtol(argv[i], &colon, 10);
		gid = (colon != NULL && *colon == ':') ?
		    strtol(colon + 1, NULL, 10) : 0;
		v = openvol(img, part, 1);
		if (nufs_chown(v, argv[i + 1], (uint16_t)uid, (uint16_t)gid) != 0)
			die(v, argv[i + 1]);
	} else if (strcmp(cmd, "rm") == 0) {
		if (i >= argc)
			usage();
		v = openvol(img, part, 1);
		if (nufs_unlink(v, argv[i]) != 0)
			die(v, argv[i]);
	} else if (strcmp(cmd, "mkdir") == 0) {
		if (i >= argc)
			usage();
		v = openvol(img, part, 1);
		if (nufs_mkdir(v, argv[i], 0755, 0, 0) != 0)
			die(v, argv[i]);
	} else if (strcmp(cmd, "rmdir") == 0) {
		if (i >= argc)
			usage();
		v = openvol(img, part, 1);
		if (nufs_rmdir(v, argv[i]) != 0)
			die(v, argv[i]);
	} else if (strcmp(cmd, "ln") == 0) {
		if (i < argc && strcmp(argv[i], "-s") == 0) {
			if (i + 2 >= argc)
				usage();
			v = openvol(img, part, 1);
			if (nufs_symlink(v, argv[i + 1], argv[i + 2], 0, 0) != 0)
				die(v, argv[i + 2]);
		} else {
			if (i + 1 >= argc)
				usage();
			v = openvol(img, part, 1);
			if (nufs_link(v, argv[i], argv[i + 1]) != 0)
				die(v, argv[i + 1]);
		}
	} else
		usage();

	if (nufs_flush(v) != 0)
		die(v, "flush");
	nufs_close(v);
	return 0;
}
