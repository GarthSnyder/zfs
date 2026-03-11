// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2022 Axcient.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2024, Klara, Inc.
 */

#include <err.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include <sys/zstd/zstd.h>
#include "zfs_fletcher.h"
#include "zstream.h"
#include "zstream_chain.h"
#include "zstream_util.h"
#include "zstream_modules.h"

#define KEYSIZE 64

static boolean_t
chain_decompress_named_writes(drr_packet_t *item, void *context,
    chain_attrs_t *attrs)
{
	(void) context;
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw = &drr->drr_u.drr_write;
	char key[KEYSIZE];

	if (item == NULL || drr->drr_type != DRR_WRITE) {
		return (B_TRUE);
	}

	snprintf(key, KEYSIZE, "%zu,%zu", drrw->drr_object, drrw->drr_offset);
	ENTRY e = { .key = key };
	ENTRY *p = hsearch(e, FIND);
	if (p == NULL) {
		return (B_TRUE);
	}

	enum zio_compress c = (enum zio_compress)(intptr_t)p->data;
	if (c == ZIO_COMPRESS_OFF) {
		drrw->drr_compressiontype = 0;
		drrw->drr_compressed_size = 0;
		if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
			fprintf(stderr,
			    "Resetting compression type to "
			    "off for ino %zu offset %zu\n",
			    drrw->drr_object, drrw->drr_offset);
		}
		return (B_TRUE);
	}

	uint64_t lsize = drrw->drr_logical_size;
	uint8_t *buff = safe_calloc(lsize);
	VERIFY3U(item->dp_payload_size, <=, lsize);

	abd_t sabd, dabd;
	abd_get_from_buf_struct(&sabd, item->dp_payload, item->dp_payload_size);
	abd_get_from_buf_struct(&dabd, buff, lsize);
	int err = zio_decompress_data(c, &sabd, &dabd,
		item->dp_payload_size, lsize, NULL);
	abd_free(&dabd);
	abd_free(&sabd);

	if (err == 0) {
		drrw->drr_compressiontype = 0;
		drrw->drr_compressed_size = 0;
		item->dp_payload_size = lsize;
		free(item->dp_payload);
		item->dp_payload = buff;
		if (OPTION_ENABLED(attrs, CA_VERBOSE)) {
			fprintf(stderr,
			    "successfully decompressed ino %zu offset %zu\n",
			    drrw->drr_object, drrw->drr_offset);
		}
	} else {
		/*
		 * The block must not be compressed, at least not with this
		 * compression type, possibly because it gets written
		 * multiple times in this stream.
		 */
		warnx("decompression failed for ino %zu offset %zu",
		    drrw->drr_object, drrw->drr_offset);
		free(buff);
	}

	return (B_TRUE);
}

static chain_step_t
serial_decompress_named_writes(void)
{
	return ((chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof(drr_packet_t),
		.cs_out_size = sizeof(drr_packet_t),
		.cs_context = NULL,
		.cs_serial = {
		    .process =
		        (zc_serial_process_f *)chain_decompress_named_writes
		}
	});
}

int
zstream_do_decompress(int argc, char *argv[])
{
	chain_attrs_t attrs = {0};
	int c;

	while ((c = getopt(argc, argv, "v")) != -1) {
		switch (c) {
		case 'v':
			ENABLE_OPTION(&attrs, CA_VERBOSE);
			break;
		case '?':
			(void) fprintf(stderr, "invalid option '%c'\n",
			    optopt);
			zstream_usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 0)
		zstream_usage();

	if (hcreate(argc) == 0)
		errx(1, "hcreate");
	for (int i = 0; i < argc; i++) {
		uint64_t object, offset;
		char *obj_str;
		char *offset_str;
		char *key;
		char *end;
		enum zio_compress type = ZIO_COMPRESS_LZ4;

		obj_str = strsep(&argv[i], ",");
		if (argv[i] == NULL) {
			zstream_usage();
			exit(2);
		}
		errno = 0;
		object = strtoull(obj_str, &end, 0);
		if (errno || *end != '\0')
			errx(1, "invalid value for object");
		offset_str = strsep(&argv[i], ",");
		offset = strtoull(offset_str, &end, 0);
		if (errno || *end != '\0')
			errx(1, "invalid value for offset");
		if (argv[i]) {
			if (0 == strcmp("off", argv[i]))
				type = ZIO_COMPRESS_OFF;
			else if (0 == strcmp("lz4", argv[i]))
				type = ZIO_COMPRESS_LZ4;
			else if (0 == strcmp("lzjb", argv[i]))
				type = ZIO_COMPRESS_LZJB;
			else if (0 == strcmp("gzip", argv[i]))
				type = ZIO_COMPRESS_GZIP_1;
			else if (0 == strcmp("zle", argv[i]))
				type = ZIO_COMPRESS_ZLE;
			else if (0 == strcmp("zstd", argv[i]))
				type = ZIO_COMPRESS_ZSTD;
			else {
				fprintf(stderr, "Invalid compression type %s.\n"
				    "Supported types are off, lz4, lzjb, gzip, "
				    "zle, and zstd\n",
				    argv[i]);
				exit(2);
			}
		}

		if (asprintf(&key, "%llu,%llu", (u_longlong_t)object,
		    (u_longlong_t)offset) < 0) {
			err(1, "asprintf");
		}
		ENTRY e = {.key = key};
		ENTRY *p;

		p = hsearch(e, ENTER);
		if (p == NULL)
			errx(1, "hsearch");
		p->data = (void*)(intptr_t)type;
	}
	ENABLE_OPTION(&attrs, CA_FORBID_DEDUP);
	zstream_chain_t decompress_chain = {
		STANDARD_INPUT_STACK(NULL, 1024),
		serial_decompress_named_writes(),
		STANDARD_OUTPUT_STACK(NULL, 512)
	};

	zstream_chain_exec(decompress_chain, attrs);
	hdestroy();
	return (0);
}
