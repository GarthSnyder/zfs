// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the Common
 * Development and Distribution License ("CDDL"), version 1.0. You may only use
 * this file in accordance with the terms of version 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this source. A
 * copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2022 Axcient.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2022 by Delphix. All rights reserved.
 * Copyright (c) 2024, Klara, Inc.
 * Copyright (c) 2026 by Garth Snyder
 */

#include <assert.h>		/* VERIFY3U, VERIFY0P, VERIFY3P		*/
#include <err.h>		/* warnx				*/
#include <stdint.h>		/* uint8_t				*/
#include <stdio.h>		/* NULL, fprintf, size_t		*/
#include <stdlib.h>		/* exit, free				*/
#include <string.h>		/* strcmp				*/
#include <sys/zfs_ioctl.h>	/* drr_write, dmu_replay_record_t	*/
#include <sys/zio.h>		/* ZIO_DATA_SALT_LEN			*/
#include <sys/zio_compress.h>	/* zio_compress, zio_compre...		*/
#include <sys/zstd/zstd.h>	/* zfs_get_hdrlevel			*/
#include <unistd.h>		/* getopt, optarg, optind		*/

#include "zstream.h"		/* zstream_usage, zstream_do_recompress	*/
#include "zstream_modules.h"	/* STANDARD_INPUT_STACK...		*/
#include "zstream_recompress.h"	/* serial_compress_writes		*/
#include "zstream_util.h"	/* compression_spec_t...		*/

#define MAX_COMPRESSION_STEPS  4

static compression_spec_t	specs[MAX_COMPRESSION_STEPS];
static int			next_spec = 0;

/*
 * These two functions determine which packets require processing. A packet
 * that's already compressed with the target profile can be skipped.
 */
static size_t
chain_compress_cost(drr_packet_t *item, compression_spec_t *context)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw	= &drr->drr_u.drr_write;
	uint8_t cur_level;

	if (drr->drr_type != DRR_WRITE) {
		return (0);
	}
	/*
	 * In order to recompress an encrypted block, you have to decrypt,
	 * decompress, recompress, and re-encrypt. That can be a future
	 * enhancement (along with decryption or re-encryption), but for now
	 * we skip encrypted blocks.
	 */
	for (int i = 0; i < ZIO_DATA_SALT_LEN; i++) {
		if (drrw->drr_salt[i] != 0) {
			return (0);
		}
	}
	if (drrw->drr_compressiontype == context->cs_type) {
		if (context->cs_type == ZIO_COMPRESS_ZSTD) {
			cur_level = zfs_get_hdrlevel((void *)item->dp_payload);
			if (context->cs_level != cur_level) {
				return (item->dp_payload_size);
			}
		}
		return (0);
	}
	return (item->dp_payload_size);
}

/*
 * Don't decompress packets that aren't compressed. And don't decompress
 * them if their ultimate fate is to be recompressed using the compression
 * profile that's already in use.
 */
static size_t
chain_decompress_cost(drr_packet_t *item, compression_spec_t *context)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw	= &drr->drr_u.drr_write;
	enum zio_compress ctype	= drrw->drr_compressiontype;

	if (drr->drr_type != DRR_WRITE ||
	    zio_compress_table[ctype].ci_decompress == NULL)
	{
		return (0);
	}
	if (context != NULL) {
		return (chain_compress_cost(item, context));
	} else {
		return (item->dp_payload_size);
	}
}

static boolean_t
chain_decompress_writes(drr_packet_t *item, compression_spec_t *context,
    chain_attrs_t *attrs)
{
	(void) attrs;
	if (item == NULL || chain_decompress_cost(item, context) == 0)
		return (B_TRUE);

	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw	= &drr->drr_u.drr_write;
	uint8_t *debuff;

	VERIFY3U(drr->drr_type, ==, DRR_WRITE);

	debuff = decompress_buffer(item->dp_payload, item->dp_payload_size,
	    drrw->drr_logical_size, drrw->drr_compressiontype);
	if (debuff == NULL) {
		warnx("Decompression type %d failed for ino %zu offset %zu",
		    drrw->drr_compressiontype,
		    drrw->drr_object,
		    drrw->drr_offset);
		exit(4);
	}
	free(item->dp_payload);
	item->dp_payload = debuff;
	item->dp_payload_size = drrw->drr_logical_size;
	drrw->drr_compressed_size = 0;
	drrw->drr_compressiontype = 0;
	return (B_TRUE);
}

static boolean_t
chain_compress_writes(drr_packet_t *item, compression_spec_t *context,
    chain_attrs_t *attrs)
{
	(void) attrs;
	if (item == NULL || chain_compress_cost(item, context) == 0)
		return (B_TRUE);

	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw	 = &drr->drr_u.drr_write;
	enum zio_compress ctype	 = drrw->drr_compressiontype;
	uint8_t *cbuff = safe_calloc(drrw->drr_logical_size);
	size_t	csize;

	VERIFY3U(drr->drr_type, ==, DRR_WRITE);
	VERIFY0P(zio_compress_table[ctype].ci_decompress);
	cbuff = compress_buffer(item->dp_payload, item->dp_payload_size,
		*context, &csize);
	if (cbuff == NULL) {
		drrw->drr_compressiontype = 0;
		drrw->drr_compressed_size = 0;
	} else {
		free(item->dp_payload);
		item->dp_payload = cbuff;
		item->dp_payload_size = csize;
		drrw->drr_compressed_size = csize;
		drrw->drr_compressiontype = context->cs_type;
	}
	return (B_TRUE);
}

/*
 * Decompress writes, but only if they don't match a target compression
 * type. Pass NULL to uncompress unconditionally (if not already
 * uncompressed).
 */
chain_step_t
serial_decompress_writes(compression_spec_t *target)
{
	int this_spec = next_spec++ % MAX_COMPRESSION_STEPS;
	compression_spec_t *context = &specs[this_spec];

	if (target == NULL) {
		context = NULL;
	} else {
		*context = *target;
	}
	return ((chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.cs_context = context,
		.cs_serial = {
			.process =
			    (zc_serial_process_f *)chain_decompress_writes
		}
	});
}

chain_step_t
serial_compress_writes(compression_spec_t *target)
{
	int this_spec = next_spec++ % MAX_COMPRESSION_STEPS;
	compression_spec_t *context = &specs[this_spec];

	VERIFY3P(target, !=, NULL);
	*context = *target;
	return ((chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.cs_context = context,
		.cs_serial = {
			.process =
			    (zc_serial_process_f *)chain_compress_writes
		}
	});
}

int
zstream_do_recompress(int argc, char *argv[])
{
	int c;
	int level = 0;
	chain_attrs_t attrs = { .ca_command_opts = CA_FORBID_DEDUP };

	while ((c = getopt(argc, argv, "l:")) != -1) {
		switch (c) {
		case 'l':
			if (sscanf(optarg, "%d", &level) != 1) {
				fprintf(stderr, "Failed to parse level '%s'\n",
				    optarg);
				zstream_usage();
			}
			break;
		case '?':
			fprintf(stderr, "invalid option '%c'\n", optopt);
			zstream_usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		zstream_usage();

	compression_spec_t spec = { .cs_level = level };
	if (strcmp(argv[0], "off") == 0) {
		spec.cs_type = ZIO_COMPRESS_OFF;
	} else {
		enum zio_compress ct;
		for (ct = 0; ct < ZIO_COMPRESS_FUNCTIONS; ct++) {
			if (!strcmp(argv[0], zio_compress_table[ct].ci_name))
				break;
		}
		if (ct == ZIO_COMPRESS_FUNCTIONS ||
		    zio_compress_table[ct].ci_compress == NULL)
		{
			fprintf(stderr, "Invalid compression type %s.\n",
			    argv[0]);
			exit(2);
		}
		spec.cs_type = ct;
	}

	zstream_chain_t recompress_chain = {
		STANDARD_INPUT_STACK(NULL),
		serial_decompress_writes(&spec),
		serial_compress_writes(&spec),
		STANDARD_OUTPUT_STACK(NULL)
	};

	zstream_chain_exec(recompress_chain, &attrs);
	return (0);
}
