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
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 * Copyright 2022 Axcient.  All rights reserved.
 * Copyright (c) 2022 by Delphix. All rights reserved.
 * Copyright (c) 2024, Klara, Inc.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include "zfs_fletcher.h"
#include "zstream.h"
#include "zstream_shared.h"
#include "zstream_stream.h"

/*
 * Byteswapping only, should be invertible through a second call.
 * This function does not byte-swap checksums.
 */
void
drr_byteswap(dmu_replay_record_t *drr, int record_type)
{
	struct drr_begin *drrb = &drr->drr_u.drr_begin;
	struct drr_end *drre = &drr->drr_u.drr_end;
	struct drr_object *drro = &drr->drr_u.drr_object;
	struct drr_freeobjects *drrfo = &drr->drr_u.drr_freeobjects;
	struct drr_write *drrw = &drr->drr_u.drr_write;
	struct drr_write_byref *drrwbr = &drr->drr_u.drr_write_byref;
	struct drr_free *drrf = &drr->drr_u.drr_free;
	struct drr_spill *drrs = &drr->drr_u.drr_spill;
	struct drr_write_embedded *drrwe = &drr->drr_u.drr_write_embedded;
	struct drr_object_range *drror = &drr->drr_u.drr_object_range;
	struct drr_redact *drrr = &drr->drr_u.drr_redact;
	struct drr_checksum *drrc = &drr->drr_u.drr_checksum;

	drr->drr_type = BSWAP_32(drr->drr_type);
	drr->drr_payloadlen = BSWAP_32(drr->drr_payloadlen);

	switch (record_type) {

	case DRR_BEGIN:
		drrb->drr_magic = BSWAP_64(drrb->drr_magic);
		drrb->drr_versioninfo = BSWAP_64(drrb->drr_versioninfo);
		drrb->drr_creation_time = BSWAP_64(drrb->drr_creation_time);
		drrb->drr_type = BSWAP_32(drrb->drr_type);
		drrb->drr_flags = BSWAP_32(drrb->drr_flags);
		drrb->drr_toguid = BSWAP_64(drrb->drr_toguid);
		drrb->drr_fromguid = BSWAP_64(drrb->drr_fromguid);
		break;

	case DRR_END:
		drre->drr_checksum.zc_word[0] = BSWAP_64(drre->drr_checksum.zc_word[0]);
		drre->drr_checksum.zc_word[1] = BSWAP_64(drre->drr_checksum.zc_word[1]);
		drre->drr_checksum.zc_word[2] = BSWAP_64(drre->drr_checksum.zc_word[2]);
		drre->drr_checksum.zc_word[3] = BSWAP_64(drre->drr_checksum.zc_word[3]);
		break;

	case DRR_OBJECT:
		drro->drr_object = BSWAP_64(drro->drr_object);
		drro->drr_type = BSWAP_32(drro->drr_type);
		drro->drr_bonustype = BSWAP_32(drro->drr_bonustype);
		drro->drr_blksz = BSWAP_32(drro->drr_blksz);
		drro->drr_bonuslen = BSWAP_32(drro->drr_bonuslen);
		drro->drr_raw_bonuslen = BSWAP_32(drro->drr_raw_bonuslen);
		drro->drr_toguid = BSWAP_64(drro->drr_toguid);
		drro->drr_maxblkid = BSWAP_64(drro->drr_maxblkid);
		break;

	case DRR_FREEOBJECTS:
		drrfo->drr_firstobj = BSWAP_64(drrfo->drr_firstobj);
		drrfo->drr_numobjs = BSWAP_64(drrfo->drr_numobjs);
		drrfo->drr_toguid = BSWAP_64(drrfo->drr_toguid);
		break;

	case DRR_WRITE:
		drrw->drr_object = BSWAP_64(drrw->drr_object);
		drrw->drr_type = BSWAP_32(drrw->drr_type);
		drrw->drr_offset = BSWAP_64(drrw->drr_offset);
		drrw->drr_logical_size = BSWAP_64(drrw->drr_logical_size);
		drrw->drr_toguid = BSWAP_64(drrw->drr_toguid);
		drrw->drr_key.ddk_prop = BSWAP_64(drrw->drr_key.ddk_prop);
		drrw->drr_compressed_size = BSWAP_64(drrw->drr_compressed_size);
		break;

	case DRR_WRITE_BYREF:
		drrwbr->drr_object = BSWAP_64(drrwbr->drr_object);
		drrwbr->drr_offset = BSWAP_64(drrwbr->drr_offset);
		drrwbr->drr_length = BSWAP_64(drrwbr->drr_length);
		drrwbr->drr_toguid = BSWAP_64(drrwbr->drr_toguid);
		drrwbr->drr_refguid = BSWAP_64(drrwbr->drr_refguid);
		drrwbr->drr_refobject = BSWAP_64(drrwbr->drr_refobject);
		drrwbr->drr_refoffset = BSWAP_64(drrwbr->drr_refoffset);
		drrwbr->drr_key.ddk_prop = BSWAP_64(drrwbr->drr_key.ddk_prop);
		break;

	case DRR_FREE:
		drrf->drr_object = BSWAP_64(drrf->drr_object);
		drrf->drr_offset = BSWAP_64(drrf->drr_offset);
		drrf->drr_length = BSWAP_64(drrf->drr_length);
		/* toguid not byte-swapped in zstream_dump.c */
		drrf->drr_toguid = BSWAP_64(drrf->drr_toguid);
		break;

	case DRR_SPILL:
		drrs->drr_object = BSWAP_64(drrs->drr_object);
		drrs->drr_length = BSWAP_64(drrs->drr_length);
		/* toguid not byte-swapped in zstream_dump.c */
		drrs->drr_toguid = BSWAP_64(drrs->drr_toguid);
		drrs->drr_compressed_size = BSWAP_64(drrs->drr_compressed_size);
		drrs->drr_type = BSWAP_32(drrs->drr_type);
		break;

	case DRR_WRITE_EMBEDDED:
		drrwe->drr_object = BSWAP_64(drrwe->drr_object);
		drrwe->drr_offset = BSWAP_64(drrwe->drr_offset);
		drrwe->drr_length = BSWAP_64(drrwe->drr_length);
		drrwe->drr_toguid = BSWAP_64(drrwe->drr_toguid);
		drrwe->drr_lsize = BSWAP_32(drrwe->drr_lsize);
		drrwe->drr_psize = BSWAP_32(drrwe->drr_psize);
		break;

	case DRR_OBJECT_RANGE:
		drror->drr_firstobj = BSWAP_64(drror->drr_firstobj);
		drror->drr_numslots = BSWAP_64(drror->drr_numslots);
		drror->drr_toguid = BSWAP_64(drror->drr_toguid);
		break;

	case DRR_REDACT:
		drrr->drr_object = BSWAP_64(drrr->drr_object);
		drrr->drr_offset = BSWAP_64(drrr->drr_offset);
		drrr->drr_length = BSWAP_64(drrr->drr_length);
		drrr->drr_toguid = BSWAP_64(drrr->drr_toguid);
		break;

	default:
		(void) fprintf(stderr, "Unknown record type, aborting...\n");
		exit(1);
	}
}

#define CALL_ALL(filter_name, record, payload, payload_size, context) \
	for (int i = 0; i < num_filters; i++) { \
		if (filters[i].filter_name) { \
			filters[i].filter_name(record, payload, payload_size, context); \
		} \
	}

int
read_stream(FILE *input, int output, stream_filter_t *filters,
	int num_filters, boolean_t retain_payload, void *context)
{
	zio_cksum_t in_cksum;
	zio_cksum_t out_cksum;
	boolean_t	first_record = B_TRUE;
	boolean_t	do_byteswap;
	uint32_t	payload_size;
	uint8_t		*payload;

	dmu_replay_record_t thedrr;
	dmu_replay_record_t *drr = &thedrr;

	struct drr_begin *drrb = &drr->drr_u.drr_begin;
	struct drr_end *drre = &drr->drr_u.drr_end;
	struct drr_object *drro = &drr->drr_u.drr_object;
	struct drr_write *drrw = &drr->drr_u.drr_write;
	struct drr_spill *drrs = &drr->drr_u.drr_spill;
	struct drr_write_embedded *drrwe = &drr->drr_u.drr_write_embedded;
	struct drr_checksum *drrc = &drr->drr_u.drr_checksum;

	fletcher_4_init();

	while (sfread(drr, sizeof(*drr), input)) {

		/*
		 * If this is the first DMU record being processed, check for
		 * the magic bytes and figure out the endian-ness based on them.
		 */
		if (first_record) {
			if (drrb->drr_magic == BSWAP_64(DMU_BACKUP_MAGIC)) {
				do_byteswap = B_TRUE;
			} else if (drrb->drr_magic != DMU_BACKUP_MAGIC) {
				(void) fprintf(stderr, "Invalid stream (bad magic number)\n");
				exit(1);
			}
			first_record = B_FALSE;
		}

		/* Validate checksum before byteswapping */
		uint32_t type = do_byteswap ? BSWAP_32(drr->drr_type) : drr->drr_type;
		if (type > DRR_NUMTYPES) {
			fprintf(stderr, "Invalid record type: %d\n", (int)type);
			exit(1);
		}
		boolean_t skip_checksum = (type == DRR_BEGIN) ||
			((type == DRR_END && drr->drr_u.drr_end.drr_toguid == 0));
		if (skip_checksum) {
			ZIO_SET_CHECKSUM(&in_cksum, 0, 0, 0, 0);
			ZIO_SET_CHECKSUM(&out_cksum, 0, 0, 0, 0);
		} else {
			if (type == DRR_END) {
				if (!ZIO_CHECKSUM_EQUAL(drre->drr_checksum, in_cksum)) {
					fprintf(stderr, "Incorrect checksum in END record.\n");
					fprintf(stderr, "Expected checksum = %llx/%llx/%llx/%llx\n",
					    (longlong_t)in_cksum.zc_word[0],
					    (longlong_t)in_cksum.zc_word[1],
					    (longlong_t)in_cksum.zc_word[2],
					    (longlong_t)in_cksum.zc_word[3]);
					exit(1);
				}
			}
			fletcher_4_incremental_native(drr,
			    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum),
			    &in_cksum);
			zio_cksum_t dup_cksum = in_cksum;
			if (do_byteswap) {
				ZIO_CHECKSUM_BSWAP(&dup_cksum);
			}
			if (!ZIO_CHECKSUM_EQUAL(drrc->drr_checksum, dup_cksum)) {
				fprintf(stderr, "Incorrect checksum in record header.\n");
				fprintf(stderr, "Expected checksum = %llx/%llx/%llx/%llx\n",
				    (longlong_t)dup_cksum.zc_word[0],
				    (longlong_t)dup_cksum.zc_word[1],
				    (longlong_t)dup_cksum.zc_word[2],
				    (longlong_t)dup_cksum.zc_word[3]);
				exit(1);
			}
			/*
			 * We need to complete the checksum's checksum and the
			 * checksum of the payload even though it's not visible to
			 * the client, just so we can validate checksums on future
			 * incoming records.
			 */
			fletcher_4_incremental_native(&drrc->drr_checksum,
				sizeof(drrc->drr_checksum), &in_cksum);
		}

		if (do_byteswap) {
			drr_byteswap(drr, BSWAP_32(drr->drr_type));
		}

		payload_size = drr->drr_payloadlen;
		switch (drr->drr_type) {
			case DRR_OBJECT:
				payload_size = DRR_OBJECT_PAYLOAD_SIZE(drro);
				break;
			case DRR_WRITE:
				payload_size = DRR_WRITE_PAYLOAD_SIZE(drrw);
				break;
			case DRR_SPILL:
				payload_size = DRR_SPILL_PAYLOAD_SIZE(drrs);
				break;
			case DRR_WRITE_EMBEDDED:
				payload_size = P2ROUNDUP(drrwe->drr_psize, 8);
				break;
			default:
				break;
		}

		if (payload_size) {
			payload = safe_malloc(payload_size);
			(void) sfread(payload, payload_size, input);
			fletcher_4_incremental_native(payload, payload_size, &in_cksum);
		}

		CALL_ALL(all_records_pre, drr, &payload, &payload_size, context);
		CALL_ALL(by_type[drr->drr_type], drr, &payload, &payload_size, context);
		CALL_ALL(all_records_post, drr, &payload, &payload_size, context);

		/*
		 * At this point all callback meddling is complete. Prepare
		 * the final versions of the checksums.
		 */

		fletcher_4_incremental_native(drr,
		    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum),
		    &out_cksum);
		skip_checksum = (drr->drr_type == DRR_BEGIN) ||
			((drr->drr_type == DRR_END && drr->drr_u.drr_end.drr_toguid == 0));
		if (!skip_checksum) {
			drrc->drr_checksum = out_cksum;
		} else {
			ZIO_SET_CHECKSUM(&drrc->drr_checksum, 0, 0, 0, 0);
		}
		fletcher_4_incremental_native(&drrc->drr_checksum,
			sizeof(drrc->drr_checksum), &out_cksum);
		if (payload_size) {
			fletcher_4_incremental_native(payload, payload_size, &out_cksum);
		}

		if (output > 0) {
			if (write(output, drr, sizeof (*drr)) == -1) {
				fprintf(stderr, "Unable to write output record.\n");
				exit(1);
			}
			if (payload_size != 0) {
				if (write(output, payload, payload_size) == -1) {
					fprintf(stderr, "Unable to write output payload.\n");
					exit(1);
				}
			}
		}
		if (!retain_payload && payload_size > 0) {
			free(payload);
		}
	}
	fletcher_4_fini();
}
