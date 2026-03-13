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

#include <errno.h>		/* errno				*/
#include <stdio.h>		/* fprintf, size_t, stderr, NULL...	*/
#include <stdlib.h>		/* exit, free, calloc, malloc		*/
#include <string.h>		/* strerror				*/
#include <assert.h>		/* VERIFY3U				*/
#include <sys/abd.h>		/* abd_free, abd_get_from_buf_struct...	*/
#include <sys/fs/zfs.h>		/* SPA_MINBLOCKSIZE			*/
#include <sys/sysmacros.h>	/* P2ROUNDUP				*/

#include "zstream_util.h"

void *
safe_malloc(size_t size)
{
	void *rv = malloc(size);
	if (rv == NULL) {
		(void) fprintf(stderr, "Error: failed to allocate %zu bytes\n",
		    size);
		exit(1);
	}
	return (rv);
}

void *
safe_calloc(size_t size)
{
	void *rv = calloc(1, size);
	if (rv == NULL) {
		(void) fprintf(stderr,
		    "Error: failed to allocate %zu bytes\n", size);
		exit(1);
	}
	return (rv);
}

/*
 * Safe version of fread(), exits on error.
 */
int
sfread(void *buf, size_t size, FILE *fp)
{
	int rv = fread(buf, size, 1, fp);
	if (rv == 0 && ferror(fp)) {
		(void) fprintf(stderr, "Error while reading file: %s\n",
		    strerror(errno));
		exit(1);
	}
	return (rv);
}

char *
checksum_str(zio_cksum_t *cksum, char *buff, size_t buff_size) {
	snprintf(buff, buff_size, "%.16llx / %.16llx / %.16llx / %.16llx",
		(long long unsigned int) cksum->zc_word[0],
		(long long unsigned int) cksum->zc_word[1],
		(long long unsigned int) cksum->zc_word[2],
		(long long unsigned int) cksum->zc_word[3]);
	return buff;
}

boolean_t
validate_checksum(zio_cksum_t *expected, zio_cksum_t *actual,
	const char *where)
{
	static char buff[128];

	if (ZIO_CHECKSUM_EQUAL(*expected, *actual)) {
		return B_TRUE;
	}
	fprintf(stderr, "Incorrect checksum %s.\n", where);
	fprintf(stderr, "Expected = %s\n", checksum_str(expected, buff,
	    sizeof(buff)));
	fprintf(stderr, "  Actual = %s\n", checksum_str(actual, buff,
	    sizeof(buff)));
	return B_FALSE;
}

/*
 * The compress_type must reflect the buffer's current compression. Returns
 * an allocated buffer if decompression was successful, NULL otherwise.
 */
uint8_t *
decompress_buffer(uint8_t *inbuff, size_t inbuff_size, size_t logical_size,
	enum zio_compress compress_type)
{
	uint8_t *outbuff = safe_malloc(logical_size);
	abd_t sabd, dabd;
	int ret;

	VERIFY3U(compress_type, !=, ZIO_COMPRESS_OFF);
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

