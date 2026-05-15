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

#include <assert.h>
#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_compress.h>
#include <sys/zstd/zstd.h>
#include <unistd.h>
#include <sys/stdtypes.h>

#include "zstream.h"
#include "zstream_chain.h"
#include "zstream_modules.h"
#include "zstream_util.h"

#define	MAX_COMPRESSION_STEPS 4

static compression_spec_t	specs[MAX_COMPRESSION_STEPS];
static int			next_spec = 0;

/*
 * Item is known to be a DRR_WRITE packet. Determine whether current
 * compression is compatible with desired compression and whether the
 * current record is modifiable at all.
 */
static boolean_t
needs_modification(drr_packet_t *item, compression_spec_t *target)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw	 = &drr->drr_u.drr_write;
	enum zio_compress ctype  = drrw->drr_compressiontype;
	uint8_t cur_level;

	/*
	 * Do not modify metadata records. It's a general stream invariant
	 * that metadata is never compressed. See comments at
	 * dmu_receive.c:flush_write_batch_impl().
	 */
	if (DMU_OT_IS_METADATA(drrw->drr_type)) {
		return (B_FALSE);
	}
	if (target == NULL) {
		return (!IS_UNCOMPRESSED(ctype) && !write_is_encrypted(drrw));
	}
	if (IS_UNCOMPRESSED(target->cs_type) && IS_UNCOMPRESSED(ctype)) {
		return (B_FALSE);
	}
	/*
	 * In order to recompress an encrypted block, you have to decrypt,
	 * decompress, recompress, and re-encrypt. That can be a future
	 * enhancement (along with decryption or re-encryption), but for now
	 * we skip encrypted blocks.
	 */
	if (write_is_encrypted(drrw)) {
		return (B_FALSE);
	}
	if (ctype != target->cs_type) {
		return (B_TRUE);
	}
	if (ZIO_COMPRESS_HASLEVEL(target->cs_type)) {
		cur_level = zfs_get_hdrlevel((void *)item->dp_payload);
		boolean_t is_zstd = target->cs_type == ZIO_COMPRESS_ZSTD;
		boolean_t dfl_level = target->cs_level == ZIO_COMPLEVEL_DEFAULT;
		if (is_zstd && dfl_level) {
			return (cur_level != ZIO_ZSTD_LEVEL_DEFAULT);
		}
		return (target->cs_level != cur_level);
	}
	return (B_FALSE);
}

static boolean_t
needs_compression(drr_packet_t *item, compression_spec_t *context)
{
	return (needs_modification(item, context));
}

/*
 * Don't decompress packets that aren't compressed. And don't decompress
 * them if their ultimate fate is to be recompressed using the compression
 * profile that's already in use.
 */
static boolean_t
needs_decompression(drr_packet_t *item, compression_spec_t *context)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw	 = &drr->drr_u.drr_write;
	enum zio_compress ctype	 = drrw->drr_compressiontype;

	if (IS_UNCOMPRESSED(ctype))
		return (B_FALSE);
	return (needs_modification(item, context));
}

static disposition_t
chain_decompress_writes(drr_packet_t *item, compression_spec_t *context)
{
	if (item == NULL)
		return (D_OK);

	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw	 = &drr->drr_u.drr_write;
	uint8_t *debuff;

	if (drr->drr_type != DRR_WRITE || !needs_decompression(item, context)) {
		return (D_OK);
	}

	debuff = decompress_buffer(item->dp_payload, item->dp_payload_size,
	    drrw->drr_logical_size, drrw->drr_compressiontype);
	if (debuff == NULL) {
		errx(4, "decompression type %d failed for ino %llu offset %llu",
		    drrw->drr_compressiontype,
		    (u_longlong_t)drrw->drr_object,
		    (u_longlong_t)drrw->drr_offset);
	}
	free(item->dp_payload);
	item->dp_payload = debuff;
	item->dp_payload_size = drrw->drr_logical_size;
	drrw->drr_compressed_size = 0;
	drrw->drr_compressiontype = 0;
	return (D_OK);
}

static disposition_t
chain_compress_writes(drr_packet_t *item, compression_spec_t *context)
{
	if (item == NULL)
		return (D_OK);

	dmu_replay_record_t *drr = &item->dp_drr;

	if (drr->drr_type != DRR_WRITE || !needs_compression(item, context)) {
		return (D_OK);
	}

	struct drr_write *drrw  = &drr->drr_u.drr_write;
	enum zio_compress ctype = drrw->drr_compressiontype;
	uint8_t *cbuff;
	size_t	csize;

	VERIFY3B(IS_UNCOMPRESSED(ctype), ==, B_TRUE);
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
	return (D_OK);
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
	chain_step_t step = {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.cs_context = context,
		.cs_serial = {
		    .process = (zc_serial_process_f *)chain_decompress_writes
		}
	};
	return (step);
}

chain_step_t
serial_compress_writes(compression_spec_t *target)
{
	int this_spec = next_spec++ % MAX_COMPRESSION_STEPS;
	compression_spec_t *context = &specs[this_spec];

	VERIFY3P(target, !=, NULL);
	*context = *target;
	chain_step_t step = {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.cs_context = context,
		.cs_serial = {
			.process =
			    (zc_serial_process_f *)chain_compress_writes
		}
	};
	return (step);
}

int
zstream_do_recompress(int argc, char *argv[])
{
	int c;
	int level = ZIO_COMPLEVEL_DEFAULT;

	chain_attrs_t attrs = { .ca_command_opts = CA_FORBID_DEDUP };

	while ((c = getopt(argc, argv, "l:")) != -1) {
		switch (c) {
		case 'l':
			if (sscanf(optarg, "%d", &level) != 1) {
				warnx("failed to parse level '%s'", optarg);
				zstream_usage();
			}
			break;
		case '?':
			warnx("invalid option '%c'", optopt);
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
			const char *ci_name = zio_compress_table[ct].ci_name;
			if (strcmp(argv[0], ci_name) == 0)
				break;
		}
		if (ct == ZIO_COMPRESS_FUNCTIONS || IS_UNCOMPRESSED(ct)) {
			errx(2, "invalid compression type %s. Use "
			    "'off' to mark records as uncompressed (without "
			    "decompressing)", argv[0]);
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
