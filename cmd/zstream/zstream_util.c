// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011, 2020 by Delphix. All rights reserved.
 * Copyright (c) 2012, Joyent, Inc. All rights reserved.
 * Copyright (c) 2012 Pawel Jakub Dawidek <pawel@dawidek.net>.
 * All rights reserved
 * Copyright (c) 2013 Steven Hartland. All rights reserved.
 * Copyright 2015, OmniTI Computer Consulting, Inc. All rights reserved.
 * Copyright 2016 Igor Kozhukhov <ikozhukhov@gmail.com>
 * Copyright (c) 2018, loli10K <ezomori.nozomu@gmail.com>. All rights reserved.
 * Copyright (c) 2019 Datto Inc.
 * Copyright (c) 2024, Klara, Inc.
 */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <search.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/abd.h>
#include <sys/fs/zfs.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/stdtypes.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio.h>
#include <sys/zio_compress.h>
#include <unistd.h>

#include "zstream_util.h"

#if defined(__linux__)
#include <linux/falloc.h>
#endif

void *
safe_malloc(size_t size)
{
	void *rv = malloc(size);
	if (rv == NULL) {
		errx(1, "failed to allocate %zu bytes, aborting...", size);
	}
	return (rv);
}

void *
safe_calloc(size_t size)
{
	void *rv = calloc(1, size);
	if (rv == NULL) {
		errx(1, "failed to allocate %zu bytes, aborting...", size);
	}
	return (rv);
}

void
safe_pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
	int error = pthread_sigmask(how, set, oldset);
	if (error != 0) {
		errno = error;
		err(1, "pthread_sigmask failed");
	}
}

pthread_t
safe_create_thread(thread_f *body, void *body_arg, const char *name,
    boolean_t detach)
{
	pthread_t tid;
	int ret;
	int name_attempts = 3;

	ret = pthread_create(&tid, NULL, body, body_arg);
	if (ret != 0) {
		errno = ret;
		err(1, "pthread_create for %s failed", name);
	}
	/*
	 * pthread_setname_np() fails randomly on some Debian systems
	 * because of difficulty reading /proc/self/task. The characteristic
	 * error is "No such file or directory." The name is just a
	 * debugging aid, so we can ignore the error, but we'll make three
	 * attempts to set it. This code previously printed a warning
	 * message, but that interferes with zstream dump output comparisons
	 * in ZTS.
	 */
	while (name_attempts-- > 0) {
		ret = pthread_setname_np(tid, name);
		if (ret == 0)
			break;
		usleep(100);
	}
	if (detach) {
		ret = pthread_detach(tid);
		if (ret != 0) {
			errno = ret;
			err(1, "failed to detach %s thread", name);
		}
	}
	return (tid);
}

void
safe_pwrite(int fd, const void *buf, size_t count, off64_t offset)
{
	ssize_t n;
	size_t done = 0;

	while (done < len) {
		ssize_t n = pwrite(fd, buf + done, len - done, offset + done);
		if (n < 0) {
			if (errno == EINTR)
				continue; /* zero progress this call, retry */
			err(1, "pwrite64() failed");
		}
		if (n == 0)
			errx(1, "pwrite of %llu bytes failed", count);
		done += (size_t)n;
	}
}

void
safe_pread(int fd, const void *buf, size_t count, off64_t offset)
{
	ssize_t n;
	size_t done = 0;

	while (done < len) {
		ssize_t n = pread(fd, buf + done, len - done, offset + done);
		if (n < 0) {
			if (errno == EINTR)
				continue; /* zero progress this call, retry */
			err(1, "pread failed");
		}
		if (n == 0)
			errx(1, "pread of %llu bytes failed", count);
		done += (size_t)n;
	}
}

char *
checksum_str(zio_cksum_t *cksum, char *buff, size_t buff_size)
{
	snprintf(buff, buff_size, "%.16llx / %.16llx / %.16llx / %.16llx",
	    (long long unsigned int) cksum->zc_word[0],
	    (long long unsigned int) cksum->zc_word[1],
	    (long long unsigned int) cksum->zc_word[2],
	    (long long unsigned int) cksum->zc_word[3]);
	return (buff);
}

boolean_t
validate_checksum(zio_cksum_t *expected, zio_cksum_t *actual,
    boolean_t swap, const char *where, off_t stream_offset)
{
	static char buff[128];
	zio_cksum_t swapped_actual;

	if (swap) {
		swapped_actual = *actual;
		actual = &swapped_actual;
		ZIO_CHECKSUM_BSWAP(actual);
	}
	/* cppcheck-suppress uninitvar */
	if (ZIO_CHECKSUM_EQUAL(*expected, *actual)) {
		return (B_TRUE);
	}
	fflush(stdout);
	fprintf(stderr, "Incorrect checksum %s (stream offset %lld)\n", where,
	    (longlong_t)stream_offset);
	fprintf(stderr, "Expected = %s\n", checksum_str(expected, buff,
	    sizeof (buff)));
	fprintf(stderr, "  Actual = %s\n", checksum_str(actual, buff,
	    sizeof (buff)));
	return (B_FALSE);
}

int
parse_compression_specifier(const char *str, compression_spec_t *spec)
{
	uint64_t val;
	int rc;

	rc = zfs_prop_string_to_index(ZFS_PROP_COMPRESSION, str, &val);
	if (rc == 0) {
		*spec = (compression_spec_t) {
			.cs_type = ZIO_COMPRESS_ALGO(val),
			.cs_level = ZIO_COMPRESS_LEVEL(val)
		};
	}
	return (rc);
}

/*
 * Parse a string of the form "OBJECT,OFFSET" or "OBJECT,OFFSET,COMPRESSION"
 * (with accept_compression == B_TRUE) and fill out a corresponding
 * record_specifier_t. Returns 0 if the string was successfully parsed.
 *
 * The rs_compression field of the record specifier struct is always
 * initialized to ZIO_COMPRESS_INHERIT, whether accept_compression is true
 * or not. If accept_compression is B_TRUE but the input string does not
 * specify compression, the value remains at ZIO_COMPRESS_INHERIT.
 */
static int
parse_record_specifier(const char *str, record_specifier_t *rec,
    boolean_t accept_compression)
{
	char static_buff[256];
	char *buff = static_buff;
	char *loc;
	char *obj_str, *offset_str, *end;
	boolean_t bad = B_TRUE;
	size_t in_size = strlen(str) + 1;

	if (in_size > sizeof (static_buff)) {
		buff = safe_malloc(in_size);
	}
	strcpy(buff, str);
	loc = buff;
	errno = 0;
	rec->rs_compression = (compression_spec_t) {
		.cs_type = ZIO_COMPRESS_INHERIT
	};

	obj_str = strsep(&loc, ",");
	if (loc == NULL)
		goto bail;
	rec->rs_object = strtoull(obj_str, &end, 0);
	if (errno != 0 || *end != '\0')
		goto bail;
	offset_str = strsep(&loc, ",");
	rec->rs_offset = strtoull(offset_str, &end, 0);
	if (errno != 0 || *end != '\0')
		goto bail;
	if (loc != NULL) {
		if (accept_compression) {
			int rc = parse_compression_specifier(loc,
			    &rec->rs_compression);
			if (rc != 0)
				goto bail;
		} else {
			goto bail;
		}
	}
	bad = B_FALSE;

bail:	if (buff != static_buff)
		free(buff);
	return (bad ? -1 : 0);
}

/*
 * Reads as many OBJECT,OFFSET[,COMPRESSION] record specifiers from the
 * command line as possible, entering them into an hcreate() hash table. The
 * OBJECT/OFFSET pairs become the keys and the compression types become the
 * values. If accept_compression is B_FALSE, ZIO_COMPRESS_INHERIT is used as
 * a placeholder value. This is also the default when accept_compression
 * is B_TRUE but no compression is specified.
 *
 * Stops at the first unparseable specifier and returns the number of
 * specifiers successfully parsed. Checks a few return codes that should
 * never fail and exits with a message if they do.
 */
int
parse_record_specifiers(int argc, char *argv[], boolean_t accept_compression)
{
	int num_parsed = 0;
	char *key;

	if (hcreate(argc) == 0)
		errx(1, "hcreate failed");

	for (int i = 0; i < argc; i++) {
		record_specifier_t spec;
		/*
		 * Check for a "--" argument used to force last argument to
		 * be treated as a filename.
		 */
		if (strcmp("--", argv[i]) == 0) {
			num_parsed++;
			break;
		}
		int rc = parse_record_specifier(argv[i], &spec,
		    accept_compression);
		if (rc != 0) {
			break;
		}
		int n_chars = asprintf(&key, "%llu,%llu",
		    (u_longlong_t)spec.rs_object,
		    (u_longlong_t)spec.rs_offset);
		if (n_chars < 0)
			err(1, "asprintf");
		ENTRY e = { .key = key };
		ENTRY *p = hsearch(e, ENTER);
		if (p == NULL)
			errx(1, "hsearch failed");
		if (!accept_compression) {
			spec.rs_compression.cs_type = ZIO_COMPRESS_INHERIT;
		}
		p->data = (void *)(intptr_t)spec.rs_compression.cs_type;
		num_parsed++;
	}
	return (num_parsed);
}

/*
 * Returns a raw zio_compress value rather than a compression_spec_t because
 * no clients are interested in compression levels. They just need to know
 * compression type.
 */
boolean_t
lookup_record_specifier(uint64_t object, uint64_t offset,
    enum zio_compress *ctype)
{
	char *key;
	boolean_t found = B_FALSE;
	int n_chars = asprintf(&key, "%llu,%llu", (u_longlong_t)object,
	    (u_longlong_t)offset);
	if (n_chars < 0)
		err(1, "asprintf");
	ENTRY e = { .key = key };
	ENTRY *p = hsearch(e, FIND);
	if (p != NULL) {
		*ctype = (enum zio_compress)(intptr_t)p->data;
		found = B_TRUE;
	}
	free(key);
	return (found);
}

void
destroy_record_specifier_hash(void)
{
	hdestroy();
}

boolean_t
write_is_encrypted(struct drr_write *drrw)
{
	for (int i = 0; i < ZIO_DATA_SALT_LEN; i++) {
		if (drrw->drr_salt[i] != 0) {
			return (B_TRUE);
		}
	}
	return (B_FALSE);
}

/*
 * The specified compress_type must reflect the buffer's actual compression.
 * Returns an allocated buffer if decompression was successful, NULL
 * otherwise.
 */
uint8_t *
decompress_buffer(uint8_t *inbuff, size_t inbuff_size, size_t logical_size,
    enum zio_compress compress_type)
{
	uint8_t *outbuff = safe_malloc(logical_size);
	abd_t sabd, dabd;
	int ret;

	VERIFY3B(ctype_is_uncompressed(compress_type), ==, B_FALSE);

	abd_get_from_buf_struct(&sabd, inbuff, inbuff_size);
	abd_get_from_buf_struct(&dabd, outbuff, logical_size);
	ret = zio_decompress_data(compress_type, &sabd, &dabd,
	    inbuff_size, abd_get_size(&dabd), NULL);

	abd_free(&dabd);
	abd_free(&sabd);

	if (ret != 0) {
		free(outbuff);
		return (NULL);
	}

	return (outbuff);
}

/*
 * Returns an allocated buffer if compression was successful, NULL
 * otherwise.
 */
uint8_t *
compress_buffer(uint8_t *inbuff, size_t inbuff_size,
    compression_spec_t compress_type, size_t *compressed_size)
{
	uint8_t *outbuff = safe_malloc(inbuff_size);
	abd_t	sabd, dabd;
	size_t	csize, rounded;

	VERIFY3B(ctype_is_uncompressed(compress_type.cs_type), ==, B_FALSE);

	abd_t *pabd = abd_get_from_buf_struct(&dabd, outbuff, inbuff_size);
	abd_get_from_buf_struct(&sabd, inbuff, inbuff_size);
	csize = zio_compress_data(compress_type.cs_type, &sabd,
	    &pabd, inbuff_size, inbuff_size, compress_type.cs_level);

	rounded = P2ROUNDUP(csize, SPA_MINBLOCKSIZE);
	if (rounded < inbuff_size) {
		abd_zero_off(pabd, csize, rounded - csize);
		*compressed_size = rounded;
	} else {
		free(outbuff);
		outbuff = NULL;
	}

	abd_free(&sabd);
	abd_free(&dabd);

	return (outbuff);
}

/*
 * Ask the filesystem (which may not be ZFS) to deallocate the storage that
 * backs a region of a regular file. This doesn't change the file size, but
 * it may/should result in the region reading as zeros.
 *
 * This is best-effort. Some systems (older FreeBSD systems in particular)
 * may not support it at all.
 *
 * Returns 0 if the whole region was punched, -1 with errno set otherwise
 * (EOPNOTSUPP if this platform or filesystem has no way to do it). Failure
 * is harmless; file contents outside the given region are never affected.
 */
static int
punch_hole(int fd, off_t offset, off_t length)
{
	if (offset < 0 || length <= 0) {
		errno = EINVAL;
		return (-1);
	}

#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE)
	/*
	 * Linux >= 2.6.38. The kernel zeroes partial blocks at the edges
	 * and deallocates whole blocks in the interior, so no alignment is
	 * required of the caller. FALLOC_FL_KEEP_SIZE is required to avoid
	 * truncation.
	 */
	return (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
	    offset, length));
#elif defined(__FreeBSD__) && defined(SPACECTL_DEALLOC)
	/*
	 * FreeBSD >= 14 (older releases have no hole punching API at
	 * all). fspacectl() handles alignment like Linux, but is allowed
	 * to complete partially, returning the remaining range; loop
	 * until it is done.
	 */
	struct spacectl_range range = {
		.r_offset = offset,
		.r_len = length,
	};

	while (range.r_len > 0) {
		if (fspacectl(fd, SPACECTL_DEALLOC, &range, 0, &range) != 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
	}
	return (0);
#elif defined(__APPLE__) && defined(F_PUNCHHOLE)
	/*
	 * macOS (APFS only; HFS+ returns ENOTSUP). Unlike Linux and
	 * FreeBSD, F_PUNCHHOLE fails with EINVAL unless the range is
	 * aligned to the filesystem block size, so on EINVAL retry with
	 * the range shrunk inward to the nearest block boundaries.
	 * That punches a subset of the requested region, which is safe.
	 */
	struct fpunchhole hole = {
		.fp_flags = 0,
		.reserved = 0,
		.fp_offset = offset,
		.fp_length = length,
	};

	if (fcntl(fd, F_PUNCHHOLE, &hole) == 0)
		return (0);
	if (errno != EINVAL)
		return (-1);

	struct stat st;
	off_t align = 4096;

	if (fstat(fd, &st) == 0 && st.st_blksize > 0 && ISP2(st.st_blksize))
		align = st.st_blksize;

	off_t start = P2ROUNDUP(offset, align);
	off_t end = P2ALIGN_TYPED(offset + length, align, off_t);

	if (end <= start) {
		errno = EINVAL;	/* range doesn't span a full block */
		return (-1);
	}

	hole.fp_offset = start;
	hole.fp_length = end - start;
	return (fcntl(fd, F_PUNCHHOLE, &hole));
#else
	(void) fd;
	errno = EOPNOTSUPP;
	return (-1);
#endif
}
