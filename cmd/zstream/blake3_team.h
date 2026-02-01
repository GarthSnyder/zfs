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
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#ifndef	_BLAKE3_TEAM_H
#define	_BLAKE3_TEAM_H

#include <stdint.h>
#include <sys/dmu.h>
#include <sys/zio_checksum.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	B3_TEAM_QUEUE_SIZE		64
#define	B3_TEAM_MIN_BATCH_SIZE	(64 * 1024)
#define B3_TEAM_MAX_THREADS		32

typedef struct b3_work_unit {
	dmu_replay_record_t	drr;
	uint8_t				*payload;
	uint64_t			payload_size;
	zio_checksum_t		blake3;
	uint64_t			sequence_num;
	boolean_t			needs_blake3;
	boolean_t			end_of_stream;
} b3_work_unit_t;

typedef struct b3_input_queue {
	b3_work_unit_t	slots[QUEUE_SIZE];
	int				head;
	int				tail;
	int				count;
	uint64_t		total_to_hash;
	mtx_t			mutex;
	cnd_t			not_full;
	cnd_t			not_empty;
} b3_input_queue_t;

typedef struct b3_output_slot {
	drr_work_unit_t	unit;
	boolean_t		full;
} b3_output_t;

typedef struct b3_output_queue {
	b3_output_t 	slots[QUEUE_SIZE];
	int				head;
	int				tail;
	uint64_t		next_output_seq;
	mtx_t			mutex;
	cnd_t			output_ready;	
} b3_output_queue_t;

static struct blake3_team {
	b3_input_queue_t	input_queue;
	b3_output_queue_t	output_queue;
	int					num_threads;
	thrd_t				threads[B3_TEAM_MAX_THREADS];
	boolean_t			initialized;
} blake3_team_t;

/*
 * Initialize the Blake3 hashing thread pool.
 * Must be called before submit or retrieve.
 */
void 
blake3_team_init(blake3_team_t *team);

/*
 * Submit a work unit for Blake3 hashing.
 * The unit will be queued and processed by worker threads.
 * Blocks if the input queue is full.
 */
void
blake3_team_enqueue(drr_work_unit_t *unit);

/*
 * Retrieve a completed work unit.
 * Work units are returned in the same order they were submitted
 * (by sequence_num), with the blake3 field filled in.
 * Blocks if the next expected unit is not yet ready.
 */
struct drr_work_unit blake3_team_retrieve();

#ifdef	__cplusplus
}
#endif

#endif	/* _BLAKE3_TEAM_H */