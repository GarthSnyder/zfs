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

#ifndef	_ZSTREAM_TEAM_H
#define	_ZSTREAM_TEAM_H

#include <stdint.h>
#include <sys/dmu.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>


#ifdef	__cplusplus
extern "C" {
#endif

/* 
 * This is a generalized multithreaded work queue for processing
 * DRR records (compressing, decompressing, hashing, etc.). Records
 * are returned in strict FIFO order.
 *
 * Only a subset of records will actually need work. The caller should
 * set the needs_processing flag to B_FALSE or B_TRUE to identify those
 * records. Records known to not need work can be fast-tracked in
 * some cases while preserving the FIFO order.
 * 
 * The perform_work function can modify the record header or payload,
 * or it can return results through the output field.
 */

typedef struct work_unit {
	dmu_replay_record_t	drr;
	uint8_t				*payload;
	uint64_t			payload_size;
	boolean_t			needs_processing;
	void				*output;
} work_unit_t;

typedef void
perform_work_t(work_unit_t *unit);

/*
 * Initialize the thread pool. Must be called before enqueue or dequeue.
 *
 * A target batch size can be set to allow threads to claim multiple
 * records at once for processing, up to a maximum of 16 records. This
 * measure can be helpful when multithreading overhead is significant in
 * comparison to work time. The batch size is based on the net amount of
 * payload data. If it's zero, records are processed individually (though
 * concurrently).
 */
void *
zstream_team_init(perform_work_t perform_work, uint64_t batch_size,
	uint64_t queue_length);

/*
 * Submit a work unit. The unit will be queued and processed by
 * worker threads. The call blocks if the input queue is full.
 */
void
zstream_team_enqueue(void *team, work_unit_t *unit);

/*
 * Retrieve a completed work unit. Work units are returned
 * in the same order they were submitted. Blocks if the next
 * unit is not yet ready.
 *
 * If zstream_team_dequeue returns a value other than zero, the
 * stream is complete. The returned item is not valid, and no
 * further calls may be made.
 */
int
zstream_team_dequeue(void *team, work_unit_t *unit);

/*
 * Notify the receiver and the team that all work items have
 * been submitted. The team will continue to function for dequeuers
 * until the end-of-stream marker has been read, at which point 
 * threads will be taken down and the team's memory released.
 */

void
zstream_team_fini(void *team);

#ifdef	__cplusplus
}
#endif

#endif	