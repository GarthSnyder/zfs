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
 * Copyright (c) 2026 by Garth Snyder
 */

#include <err.h>		/* errx, err, warnx			*/
#include <errno.h>		/* errno				*/
#include <search.h>		/* ENTRY, hsearch, hcreate, hdestroy	*/
#include <stdint.h>		/* intptr_t, uint64_t, uint8_t		*/
#include <stdio.h>		/* NULL, fprintf, stderr, asprintf	*/
#include <stdlib.h>		/* exit, free, strtoull			*/
#include <string.h>		/* strcmp, strsep			*/
#include <sys/stdtypes.h>	/* B_TRUE, u_longlong_t, boolean_t	*/
#include <sys/zfs_ioctl.h>	/* drr_write, dmu_replay_record...	*/
#include <sys/zio_compress.h>	/* zio_compress				*/
#include <unistd.h>		/* getopt, optind, optopt		*/

#include "zstream.h"		/* zstream_do_decompress...		*/
#include "zstream_chain.h"	/* zc_serial_process_f, CA_VERBOSE	*/
#include "zstream_modules.h"	/* STANDARD_INPUT_STACK...		*/
#include "zstream_util.h"	/* decompress_buffer			*/

#define KEYSIZE 64

static disposition_t
chain_decompress_named_writes(drr_packet_t *item, void *context)
{
	(void) context;
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw = &drr->drr_u.drr_write;
	char key[KEYSIZE];
	uint8_t *dcbuff;

	if (item == NULL || drr->drr_type != DRR_WRITE) {
		return (D_OK);
	}

	snprintf(key, KEYSIZE, "%zu,%zu", drrw->drr_object, drrw->drr_offset);
	ENTRY e = { .key = key };
	ENTRY *p = hsearch(e, FIND);
	if (p == NULL) {
		return (D_OK);
	}

	enum zio_compress ctype = (enum zio_compress)(intptr_t)p->data;
	if (IS_UNCOMPRESSED(ctype)) {
		drrw->drr_compressiontype = 0;
		drrw->drr_compressed_size = 0;
		if (OPTION_ENABLED(chain_attrs, CA_VERBOSE)) {
			fprintf(stderr,
			    "Resetting compression type to "
			    "off for ino %zu offset %zu\n",
			    drrw->drr_object, drrw->drr_offset);
		}
		return (D_OK);
	}

	if (write_is_encrypted(drrw)) {
		fprintf(stderr, "The write for ino %zu offset %zu is marked "
		    "as being encrypted. Attempting decompression anyway...\n",
		    drrw->drr_object, drrw->drr_offset);
	}

	dcbuff = decompress_buffer(item->dp_payload, item->dp_payload_size,
		drrw->drr_logical_size, ctype);

	if (dcbuff == NULL) {
		/*
		 * The block must not be compressed, at least not with this
		 * compression type, possibly because it gets written
		 * multiple times in this stream.
		 */
		warnx("decompression failed for ino %zu offset %zu",
		    drrw->drr_object, drrw->drr_offset);
		free(dcbuff);
	} else {
		free(item->dp_payload);
		item->dp_payload = dcbuff;
		item->dp_payload_size = drrw->drr_logical_size;
		drrw->drr_compressiontype = 0;
		drrw->drr_compressed_size = 0;
		if (OPTION_ENABLED(chain_attrs, CA_VERBOSE)) {
			fprintf(stderr,
			    "Successfully decompressed ino %zu offset %zu\n",
			    drrw->drr_object, drrw->drr_offset);
		}
	}
	return (D_OK);
}

static chain_step_t
serial_decompress_named_writes(void)
{
	return ((chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
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
		ENTRY e = { .key = key };
		ENTRY *p;

		p = hsearch(e, ENTER);
		if (p == NULL)
			errx(1, "hsearch");
		p->data = (void*)(intptr_t)type;
	}
	ENABLE_OPTION(&attrs, CA_FORBID_DEDUP);
	zstream_chain_t decompress_chain = {
		STANDARD_INPUT_STACK(NULL),
		serial_decompress_named_writes(),
		STANDARD_OUTPUT_STACK(NULL)
	};

	zstream_chain_exec(decompress_chain, &attrs);
	hdestroy();
	return (0);
}
