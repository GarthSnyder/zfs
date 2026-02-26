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

#include <stdint.h>
#include <sys/dmu.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>

#ifndef _ZSTREAM_HASH_STATS_H
#define _ZSTREAM_HASH_STATS_H

#ifdef  __cplusplus
extern "C" {
#endif

#include "zstream_hash.h"

/*
 * MAX_BUCKET_CHAINS is a reporting/stats limitation, not an implementation
 * limitation. There is no actual limit on the length of bucket chains.
 */
#define MAX_BUCKET_CHAIN 16

typedef struct {
	uint64_t	bs_num_chains;
	uint64_t	bs_num_empty_chains;
	uint64_t	bs_num_slots_filled;
	double		bs_pct_empty;
	double		bs_occupancy;          /* Full slots / All slots */
	double		bs_nonempty_occupancy;
} bucket_stats_t;

typedef struct {
	uint64_t	os_count;	/* Inserts, splits, etc. */
	uint64_t	os_num_io_ops;	/* Total # of reads/writes */
} op_stats_t;

typedef struct lh_report {
	bucket_stats_t	lr_chains_by_length[MAX_BUCKET_CHAIN];
	uint64_t	lr_total_entries;
	uint64_t	lr_total_chains;
	uint64_t	lr_bytes_in_data;
	uint64_t	lr_bytes_in_buckets;
	op_stats_t	lr_splits;
	op_stats_t	lr_inserts;
	op_stats_t	lr_retrieves;
	double		lr_occupancy;        /* Entries / number of chains */
	double		lr_overall_occupancy;    /* Full slots / Total slots */
} lh_report_t;

void
lh_get_stats(linear_hash_t *lh, lh_report_t *stats);

void
lh_print_stats(linear_hash_t *lh);

uint64_t
lh_get_mem_highwater(linear_hash_t *lh);

#ifdef __cplusplus
}
#endif

#endif /* _ZSTREAM_HASH_STATS_H */
