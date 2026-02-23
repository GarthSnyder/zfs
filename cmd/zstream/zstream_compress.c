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
 * Copyright (c) 2022 by Delphix. All rights reserved.
 * Copyright (c) 2024, Klara, Inc.
 */

#include <err.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include <sys/zstd/zstd.h>
#include "zfs_fletcher.h"
#include "zstream.h"
#include "zstream_shared.h"

#define MAX_COMPRESSION_STEPS  4

typedef struct {
	enum zio_compress	cs_type;
	int 			cs_level;
} compression_spec_t;

static compression_spec_t	specs[MAX_COMPRESSION_STEPS];
static int			next_spec = 0;

int
zstream_do_recompress(int argc, char *argv[])
{
	int c;
	int level = 0;

	while ((c = getopt(argc, argv, "l:")) != -1) {
		switch (c) {
		case 'l':
			if (sscanf(optarg, "%d", &level) != 1) {
				fprintf(stderr,
				    "failed to parse level '%s'\n",
				    optarg);
				zstream_usage();
			}
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

	if (argc != 1)
		zstream_usage();

	compression_spec_t spec = { .cs_level = level };
	if (strcmp(argv[0], "off") == 0) {
		spec->cs_type = ZIO_COMPRESS_OFF;
	} else {
		enum zio_compress ct;
		for (ct = 0; ct < ZIO_COMPRESS_FUNCTIONS; ct++) {
		    if (strcmp(argv[0], zio_compress_table[ct].ci_name) == 0)
			break;
		}
		if (ct == ZIO_COMPRESS_FUNCTIONS ||
		    zio_compress_table[ct].ci_compress == NULL)
		{
			fprintf(stderr, "Invalid compression type %s.\n",
				argv[0]);
			exit(2);
		}
		spec->cs_type = ct;
	}

	abd_init();
	zio_init();
	zstd_init();

	zstream_chain_t recompress_chain = {
		serial_read_stream((argc == 1) ? argv[0] : NULL),
		parallel_calc_fletcher4(),
		serial_validate_fletcher4(),
		serial_byteswap(),
		serial_validate_records(),
		parallel_decompress_writes(spec),
		parallel_compress_writes(spec),
		parallel_calc_fletcher4(),
		serial_add_fletcher4(),
		serial_write_stream(NULL)
	};

	zstream_chain_exec(recompress_chain, &attrs,
		sizeof(recompress_chain) / sizeof(chain_step_t));

	fletcher_4_fini();
	zio_fini();
	zstd_fini();
	abd_fini();
	return 0;
}

/*
 * We can ignore the context here because it's already been evaluated by the
 * cost function. If the cost function returned something other than zero,
 * we have to decompress.
 */
static void
chain_decompress_writes(drr_packet_t *item, void *context)
{
	(void) context;

	drr_packet_t *base	 = &item->dp_base;
	dmu_replay_record_t *drr = &base->dp_drr;
	struct drr_write *drrw   = &drr->drr_u.drr_write;
	enum zio_compress dtype  = drrw->drr_compressiontype;
	uint8_t *buff 		 = safe_calloc(drrw->drr_logical_size);
	abd_t sabd, dabd;

	VERIFY3U(drr->drr_type, ==, DRR_WRITE);
	VERIFY3U(drr->drr_compressiontype, !=, ZIO_COMPRESS_OFF);
	abd_get_from_buf_struct(&sabd, item->dp_base.dp_payload,
		item->dp_base.dp_payload_size);
	abd_get_from_buf_struct(&dabd, buff, drrw->drr_logical_size);
	if (zio_decompress_data(dtype, &sabd, &dabd, payload_size,
		abd_get_size(&dabd), NULL) != 0)
	{
		warnx("Decompression type %d failed "
		    "for ino %llu offset %llu",
		    dtype,
		    (u_longlong_t)drrw->drr_object,
		    (u_longlong_t)drrw->drr_offset);
		exit(4);
	}
	free(base->dp_payload);
	base->dp_payload = buff;
	base->dp_payload_size = drrw->drr_logical_size;
	drr->drr_payloadlen = drrw->drr_logical_size;
	drr->drr_compressed_size = 0;
	drr->drr_compressiontype = ZIO_COMPRESS_OFF;
	abd_free(&dabd);
	abd_free(&sabd);
}

static void
chain_compress_writes(drr_packet_t *item, compression_spec_t *context)
{
	drr_packet_t *base	 = &item->dp_base;
	dmu_replay_record_t *drr = &base->dp_drr;
	struct drr_write *drrw   = &drr->drr_u.drr_write;
	enum zio_compress dtype  = drrw->drr_compressiontype;
	uint8_t *buff 		 = safe_calloc(drrw->drr_logical_size);

	abd_t	sabd, dabd;
	size_t	csize, rounded;

	VERIFY3U(drr->drr_type, ==, DRR_WRITE);
	VERIFY3U(drrw->drr_compressiontype, ==, ZIO_COMPRESS_OFF);
	abd_t *pabd = abd_get_from_buf_struct(&dabd, buff,
		drrw->drr_logical_size);
	abd_get_from_buf_struct(&sabd, item->dp_payload,
		item->dp_payload_size);
	csize = zio_compress_data(context->cs_type, &sabd,
	    &pabd, drrw->drr_logical_size, drrw->drr_logical_size,
	    context->cs_level);
	rounded = P2ROUNDUP(csize, SPA_MINBLOCKSIZE);
	if (rounded < drrw->drr_logical_size) {
		abd_zero_off(pabd, csize, rounded - csize);
		drrw->drr_compressiontype = context->cs_type;
		drrw->drr_compressed_size = rounded;
		free(item->dp_payload);
		item->dp_payload = buff;
		item->dp_payload_size = rounded;
		drr->drr_payloadlen = rounded;
	} else {
		free(buff);
		drrw->drr_compressiontype = ZIO_COMPRESS_OFF;
		drrw->drr_compressed_size = 0;
	}
	abd_free(&sabd);
	abd_free(&dabd);
}

size_t
chain_decompress_cost(drr_packet_t *item, compression_spec_t *context)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw	 = &drr->drr_u.drr_write;
	uint8_t zstd_level;

	if (drr->drr_type != DRR_WRITE ||
		drrw->drr_compressiontype == ZIO_COMPRESS_OFF)
	{
		return (0);
	}
	if (!context) {
		return (item->dp_payload_size);
	}
	return chain_compress_cost(item, context);
}

size_t
chain_compress_cost(drr_packet_t *item, compression_spec_t *context)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw	 = &drr->drr_u.drr_write;
	uint8_t zstd_level;

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
			zfs_zstd_get_level(item->dp_payload,
				item->dp_payload_size, &cur_level);
			if (context->cs_level != cur_level) {
				return (item->dp_payload_size);
			}
		}
		return (0);
	}
	return (item->dp_payload_size);
}

/*
 * Decompress writes, but only if they don't match a target compression type.
 * Pass NULL to uncompress unconditionally (if not already uncompressed).
 */
chain_step_t
parallel_decompress_writes(compression_spec_t *target)
{
	int this_spec = next_spec % MAX_COMPRESSION_STEPS;
	compression_spec_t *context = &contexts[this_spec];

	next_spec++;
	if (!target) {
		context = NULL;
	} else {
		*context = target
	};
	return (chain_step_t) {
		.cs_type = CS_PARALLEL,
		.cs_in_size = sizeof(drr_packet_t),
		.cs_out_size = sizeof(drr_packet_t),
		.parallel = {
		    .csp_queue_length = 128,
		    .csp_batch_budget = 128 * 1024,
		    .csp_process = (zq_process_item_f *)chain_decompress_writes,
		    .csp_cost = (zq_estimate_cost_f *)chain_decompress_cost,
		    .csp_context = context
		}
	};
}

chain_step_t
parallel_compress_writes(compression_spec_t target)
{
	int this_spec = next_spec % MAX_COMPRESSION_STEPS;
	compression_spec_t *context = &contexts[this_spec];

	next_spec++;
	*context = target;
	return (chain_step_t) {
		.cs_type = CS_PARALLEL,
		.cs_in_size = sizeof(drr_packet_t),
		.cs_out_size = sizeof(drr_packet_t),
		.parallel = {
		    .csp_queue_length = 128,
		    .csp_batch_budget = 128 * 1024,
		    .csp_process = (zq_process_item_f *)chain_compress_writes,
		    .csp_cost = (zq_estimate_cost_f *)chain_compress_cost,
		    .csp_context = context
		}
	};
}
