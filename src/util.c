#include "nextufs.h"
#include <errno.h>
#include <stdarg.h>
#include <string.h>

uint32_t
nufs_get32(const struct nufs *v, const void *base, int off)
{
	const uint8_t *p = (const uint8_t *)base + off;

	if (v->be)
		return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		    ((uint32_t)p[2] << 8) | p[3];
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
	    ((uint32_t)p[1] << 8) | p[0];
}

uint16_t
nufs_get16(const struct nufs *v, const void *base, int off)
{
	const uint8_t *p = (const uint8_t *)base + off;

	return v->be ? (uint16_t)((p[0] << 8) | p[1])
		     : (uint16_t)((p[1] << 8) | p[0]);
}

void
nufs_put32(const struct nufs *v, void *base, int off, uint32_t x)
{
	uint8_t *p = (uint8_t *)base + off;

	if (v->be) {
		p[0] = x >> 24; p[1] = x >> 16; p[2] = x >> 8; p[3] = x;
	} else {
		p[3] = x >> 24; p[2] = x >> 16; p[1] = x >> 8; p[0] = x;
	}
}

void
nufs_put16(const struct nufs *v, void *base, int off, uint16_t x)
{
	uint8_t *p = (uint8_t *)base + off;

	if (v->be) {
		p[0] = x >> 8; p[1] = x;
	} else {
		p[1] = x >> 8; p[0] = x;
	}
}

int
nufs_pread(struct nufs *v, void *buf, long long off, size_t n)
{
	if (fseeko(v->f, (off_t)off, SEEK_SET) != 0)
		return nufs_err(v, "seek to %lld failed", off), -1;
	if (fread(buf, 1, n, v->f) != n)
		return nufs_err(v, "short read of %zu at %lld", n, off), -1;
	return 0;
}

int
nufs_pwrite(struct nufs *v, const void *buf, long long off, size_t n)
{
	if (!v->rw)
		return nufs_errc(v, EROFS, "volume opened read-only"), -1;
	if (fseeko(v->f, (off_t)off, SEEK_SET) != 0)
		return nufs_err(v, "seek to %lld failed", off), -1;
	if (fwrite(buf, 1, n, v->f) != n)
		return nufs_err(v, "short write of %zu at %lld", n, off), -1;
	return 0;
}

void
nufs_err(struct nufs *v, const char *fmt, ...)
{
	va_list ap;

	v->errnum = EIO;
	va_start(ap, fmt);
	vsnprintf(v->err, sizeof(v->err), fmt, ap);
	va_end(ap);
}

/* As nufs_err, but records the errno a caller should report. */
void
nufs_errc(struct nufs *v, int errnum, const char *fmt, ...)
{
	va_list ap;

	v->errnum = errnum;
	va_start(ap, fmt);
	vsnprintf(v->err, sizeof(v->err), fmt, ap);
	va_end(ap);
}
