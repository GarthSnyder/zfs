// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#ifndef	_ZSTREAM_UTIL_H
#define	_ZSTREAM_UTIL_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include <sys/zio_compress.h>

typedef struct {
	enum zio_compress	cs_type;
	int			cs_level;
} compression_spec_t;

extern uint32_t num_pthreads;

/*
 * The safe_ versions of the functions below terminate the process if the
 * operation doesn't succeed instead of returning an error.
 */
extern void *
safe_malloc(size_t size);

extern void *
safe_calloc(size_t n);

void
safe_pwrite(int fd, const void *buf, size_t count, off64_t offset);

void
safe_pread(int fd, void *buf, size_t count, off64_t offset);

extern char *
checksum_str(zio_cksum_t *cksum, char *buff, size_t buff_size);

/*
 * Prints an error message if checksums don't match. Returns B_TRUE for
 * a match, B_FALSE otherwise.
 */
boolean_t
validate_checksum(zio_cksum_t *expect, zio_cksum_t *actual, boolean_t swap,
    const char *where, off_t stream_offset);

static inline void
validate_or_exit(zio_cksum_t *expect, zio_cksum_t *actual, boolean_t swap,
    const char *where, off_t stream_offset)
{
	if (!validate_checksum(expect, actual, swap, where, stream_offset)) {
		exit(1);
	}
}

/*
 * Determine whether a compression type indicates no compression
 */
static inline boolean_t
ctype_is_uncompressed(enum zio_compress ct)
{
	VERIFY3U((int)ct, <, (int)ZIO_COMPRESS_FUNCTIONS);
	return (zio_compress_table[(int)(ct)].ci_compress == NULL);
}

boolean_t
write_is_encrypted(struct drr_write *drrw);

uint8_t *
decompress_buffer(uint8_t *inbuff, size_t inbuff_size, size_t logical_size,
	enum zio_compress compress_type);

uint8_t *
compress_buffer(uint8_t *inbuff, size_t inbuff_size,
    compression_spec_t compress_type, size_t *compressed_size);

void
pthread_register_self(void);

/*
 * Ask the filesystem (which may not be ZFS) to deallocate the storage that
 * backs a region of a regular file. This doesn't change the file size, but
 * it may/should result in the region reading as zeros.
 *
 * This is best-effort. Some systems (older FreeBSD in particular) may not
 * support it at all.
 *
 * Returns 0 if the whole region was punched, -1 with errno set otherwise
 * (EOPNOTSUPP if this platform or filesystem has no way to do it). Failure
 * is harmless; file contents outside the given region are never affected.
 */
int
punch_hole(int fd, off_t offset, off_t length);

#ifdef __cplusplus
}
#endif

#endif  /* _ZSTREAM_UTIL_H */
