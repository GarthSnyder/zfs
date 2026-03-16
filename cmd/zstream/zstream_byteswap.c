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
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/zfs_ioctl.h>

#include "zstream.h"
#include "zstream_byteswap.h"
#include "zstream_util.h"

static boolean_t
chain_btyeswap(drr_packet_t *item, void *context, chain_attrs_t chain)
{
	(void) context;
	struct dmu_replay_record *drr	 = &item->dp_drr;
	struct drr_begin *drrb		 = &drr->drr_u.drr_begin;
	struct drr_end *drre 		 = &drr->drr_u.drr_end;
	struct drr_object *drro 	 = &drr->drr_u.drr_object;
	struct drr_freeobjects *drrfo 	 = &drr->drr_u.drr_freeobjects;
	struct drr_write *drrw 		 = &drr->drr_u.drr_write;
	struct drr_write_byref *drrwbr 	 = &drr->drr_u.drr_write_byref;
	struct drr_free *drrf 		 = &drr->drr_u.drr_free;
	struct drr_spill *drrs 		 = &drr->drr_u.drr_spill;
	struct drr_write_embedded *drrwe = &drr->drr_u.drr_write_embedded;
	struct drr_object_range *drror 	 = &drr->drr_u.drr_object_range;
	struct drr_redact *drrr 	 = &drr->drr_u.drr_redact;

	if (!item || !(chain->ca_flags & CA_BYTESWAPPED)) {
		return B_TRUE;
	}
	drr->drr_type = BSWAP_32(drr->drr_type);
	drr->drr_payloadlen = BSWAP_32(drr->drr_payloadlen);

	switch (drr->drr_type) {

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
		ZIO_CHECKSUM_BSWAP(&drre->drr_checksum);
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
	return B_TRUE;
}

chain_step_t
serial_byteswap(void)
{
	return (chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof(drr_packet_t),
		.cs_out_size = sizeof(drr_packet_t),
		.cs_serial = {
			.css_process = (zc_serial_process_f *)chain_btyeswap,
		}
	};
}
