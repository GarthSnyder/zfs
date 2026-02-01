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
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>


#ifdef	__cplusplus
extern "C" {
#endif

#define	B3_TEAM_QUEUE_SIZE		64
#define	B3_TEAM_MIN_BATCH_SIZE	(64 * 1024)

typedef struct b3_work_unit {
	dmu_replay_record_t	drr;
	uint8_t				*payload;
	uint64_t			payload_size;
	zio_cksum_t			blake3_cksum;
	uint64_t			sequence_num;
	boolean_t			needs_blake3;
	boolean_t			end_of_stream;
	boolean_t			completed;
} b3_work_unit_t;

/* 
 * The uniqueue is a circular buffer with four pointers: insert,
 * claim, complete, and dequeue, in that order. No pointer can
 * move beyond its preceding pointer. Every interval between pointers
 * corresponds to a specific state that applies to the intervening
 * work items: enqueued, claimed for work, completed.
 * FIFO order is guaranteed everywhere.
 */

typedef struct b3_uniqueue {
	b3_work_unit_t	slots[B3_TEAM_QUEUE_SIZE];
	uint64_t		insert, claim, complete, dequeue;
	cnd_t			inserted;
	cnd_t			claimed;
	cnd_t			completed;
	cnd_t			dequeued;
	mtx_t			mutex;
	uint64_t		bytes_pending;
	boolean_t		terminate;
} b3_uniqueue_t;

typedef struct blake3_team {
	b3_uniqueue_t	queue;
	int				num_threads;
	thrd_t			*threads;
	boolean_t		initialized;
} blake3_team_t;

/*
 * Initialize the Blake3 hashing thread pool.
 * Must be called before submit or retrieve.
 */
void 
blake3_team_init(blake3_team_t *team);

/*
 * Submit a work unit for Blake3 hashing. The unit will be
 * queued and processed by worker threads. The call blocks
 * if the input queue is full.
 *
 * If a b3_work_unit with the end_of_stream flag is enqueued,
 * the call blocks until both input and output queues are empty
 * and then deconstructs the queue and kills the threads. 
 */
void
blake3_team_enqueue(blake3_team_t *team, b3_work_unit_t *unit);

/*
 * Retrieve a completed work unit. Work units are returned
 * in the same order they were submitted (by sequence_num),
 * with the blake3 field filled in. Blocks if the next expected
 * unit is not yet ready.
 */
b3_work_unit_t *
blake3_team_dequeue(blake3_team_t *team);

void
blake3_team_destroy(blake3_team_t *team);

#ifdef	__cplusplus
}
#endif

#endif	/* _BLAKE3_TEAM_H */