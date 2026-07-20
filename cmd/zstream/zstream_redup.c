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
 * Copyright (c) 2020 by Delphix. All rights reserved.
 */

#include <assert.h>
#include <cityhash.h>
#include <err.h>
#include <errno.h>
#include <libzutil.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/bitops.h>
#include <sys/param.h>
#include <sys/stdtypes.h>
#include <sys/sysmacros.h>
#include <sys/zfs_ioctl.h>
#include <umem.h>
#include <unistd.h>

#include "zstream.h"
#include "zstream_hash.h"
#include "zstream_modules.h"
#include "zstream_util.h"

#define	HASH_PHYSMEM_PERCENT			40
#define	SMALLEST_POSSIBLE_HASH_MEMORY_MB	128

typedef struct {
	uint64_t		rhe_guid;
	uint64_t		rhe_object;
	uint64_t		rhe_offset;
	uint64_t		rhe_stream_offset;
} redup_hash_entry_t;

typedef struct {
	linear_hash_t	*rc_hash;
	FILE		*rc_fp;
} redup_context_t;

static void
rdt_insert(linear_hash_t *lh,
    uint64_t guid, uint64_t object, uint64_t offset, uint64_t stream_offset)
{
	uint64_t hashcode = cityhash3(guid, object, offset);
	redup_hash_entry_t re = {
		.rhe_guid = guid;
		.rhe_object = object;
		.rhe_offset = offset;
		.rhe_stream_offset = stream_offset;
	}
	lh_insert(lh, hashcode, &re);
}

static void
rdt_lookup(linear_hash_t *lh, uint64_t guid, uint64_t object, uint64_t offset,
    uint64_t *stream_offsetp)
{
	uint64_t hashcode = cityhash3(guid, object, offset);
	redup_hash_entry_t entry;

	lh_iterator_t *iter = lh_initiate_retrieve(lh, hashcode);
	while (lh_retrieve_next(iter, &entry)) {
		boolean_t matches = entry.rhe_guid == guid &&
		    entry.rhe_object == object && entry.rhe_offset == offset;
		if (matches) {
			*stream_offsetp = entry.rhe_stream_offset;
			return
		}
	}
	errx(1, "could not find expected redup table entry");
}

static disposition_t
chain_redup_writes(void *item_in, void *context_in)
{
	drr_packet_t *item = (drr_packet_t *)item_in;
	redup_context_t *context = (redup_context_t *)context_in;

	if (item == NULL) {
		return (D_OK);
	}

	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw	 = &drr->drr_u.drr_write;
	struct drr_begin *drrb	 = &drr->drr_u.drr_begin;

	switch (drr->drr_type) {

	case DRR_BEGIN:
	{
		uint64_t flags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
		flags &= ~(DMU_BACKUP_FEATURE_DEDUP |
		    DMU_BACKUP_FEATURE_DEDUPPROPS);
		DMU_SET_FEATUREFLAGS(drrb->drr_versioninfo, flags);
		break;
	}

	case DRR_WRITE_BYREF:
	{
		struct drr_write_byref drrwb = drr->drr_u.drr_write_byref;

		/*
		 * Look up in hash table by drrwb->drr_refguid,
		 * drr_refobject, drr_refoffset. Replace this
		 * record with the found WRITE record, but with
		 * drr_object,drr_offset,drr_toguid replaced with ours.
		 */
		uint64_t stream_offset = 0;
		rdt_lookup(context->rc_hash, drrwb.drr_refguid,
		    drrwb.drr_refobject, drrwb.drr_refoffset,
		    &stream_offset);

		if (fseeko(context->rc_fp, stream_offset, SEEK_SET) != 0) {
			err(1, "seek into source file failed, offset %llu",
			    (u_longlong_t)stream_offset);
		}
		if (fread(drr, sizeof (*drr), 1, context->rc_fp) != 1) {
			err(1, "read of prior write failed");
		}
		if (ATTR_IS_SET(CA_BYTESWAPPED)) {
			byteswap_record(drr, BSWAP_32(drr->drr_type));
		}

		VERIFY3U(drr->drr_type,    ==, DRR_WRITE);
		VERIFY3U(drrw->drr_toguid, ==, drrwb.drr_refguid);
		VERIFY3U(drrw->drr_object, ==, drrwb.drr_refobject);
		VERIFY3U(drrw->drr_offset, ==, drrwb.drr_refoffset);

		item->dp_payload_size = DRR_WRITE_PAYLOAD_SIZE(drrw);
		item->dp_payload = safe_malloc(item->dp_payload_size);

		size_t n_read = fread(item->dp_payload, item->dp_payload_size,
		    1, context->rc_fp);
		if (n_read != 1)
			err(1, "read of prior payload failed");

		drrw->drr_toguid = drrwb.drr_toguid;
		drrw->drr_object = drrwb.drr_object;
		drrw->drr_offset = drrwb.drr_offset;
		break;
	}

	case DRR_WRITE:
		rdt_insert(context->rc_hash, drrw->drr_toguid, drrw->drr_object,
		    drrw->drr_offset, item->dp_stream_offset);
		break;

	default:
		break;
	}
	return (D_OK);
}

static chain_step_t
serial_redup_writes(redup_context_t *context)
{
	chain_step_t step = {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.cs_context = context,
		.cs_serial = {
			.process = chain_redup_writes
		}
	};
	return (step);
}

int
zstream_do_redup(int argc, char *argv[])
{
	int c;
	chain_attrs_t attrs = {0};
	redup_context_t context = {0};
	uint64_t numbuckets;
	char *temp_dir = "/var/tmp";

	while ((c = getopt(argc, argv, "vd:")) != -1) {
		switch (c) {
		case 'v':
			ENABLE_OPTION(&attrs, CA_VERBOSE);
			break;
		case 'd':
			temp_dir = optarg;
			break;
		case ':':
			warnx("missing argument for '%c' option\n", optopt);
			zstream_usage();
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

	context.rc_fp = fopen(argv[0], "rb");
	if (context.rc_fp == NULL) {
		err(1, "unable to open %s", argv[0]);
	}

#ifdef _ILP32
	uint64_t hash_memory = SMALLEST_POSSIBLE_HASH_MEMORY_MB << 20;
#else
	uint64_t physbytes = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
	uint64_t hash_memory = MAX((physbytes * HASH_PHYSMEM_PERCENT) / 100,
	    SMALLEST_POSSIBLE_HASH_MEMORY_MB << 20);
#endif

	context.rc_hash =
	    lh_init(sizeof (redup_hash_entry_t), hash_memory, temp_dir);

	zstream_chain_t redup_chain = {
		STANDARD_INPUT_STACK(argv[0]),
		serial_redup_writes(&context),
		STANDARD_OUTPUT_STACK(NULL)
	};
	zstream_chain_exec(redup_chain, &attrs);

	if (attrs.ca_command_opts & CA_VERBOSE) {
		char mem_str[16];
		record_stats_t *acsi = attrs.ca_stats_in;
		zfs_nicenum(context.rc_rdt.ddt_count * sizeof (redup_entry_t),
		    mem_str, sizeof (mem_str));
		fprintf(stderr, "Converted stream with %llu total records, "
		    "including %llu dedup records, using %sB memory.\n",
		    (u_longlong_t)attrs.ca_totals_in.rs_num_records,
		    (u_longlong_t)acsi[DRR_WRITE_BYREF].rs_num_records,
		    mem_str);
	}

	fclose(context.rc_fp);
	lh_destroy(context.rc_hash);
	return (0);
}
