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

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <sys/zstream.h>
#include "zstream_team.h"
#include "zstream_shared.h"

#define MIN_THREADS 6
#define MAX_BATCH 16

#define NEEDS_HASH(unit) (unit->drr.drr_type == DRR_WRITE && 
	unit->payload_size > 0)

/* 
 * The uniqueue is a circular buffer with four pointers: insert,
 * claim, complete, and dequeue, in that order. No pointer can
 * move beyond its preceding pointer. Every interval between pointers
 * corresponds to a specific state that applies to the intervening
 * work items: enqueued, claimed for work, completed.
 * FIFO order is guaranteed everywhere.
 *
 * No "claimed" condition is necessary because the next state
 * in line is "completed". That transition is where the actual work
 * is done, so the clock on that state is "when the hashing is done"
 * rather than a gating condition.
 */

typedef struct queue_slot {
	work_unit_t	unit;
	boolean_t	completed;
	boolean_t	end_of_stream;
} queue_slot_t;

typedef struct uniqueue {
	queue_slot_t		*slots;
	uint64_t		insert, claim, complete, dequeue;
	cnd_t			inserted, completed, dequeued;
	mtx_t			mutex;
	uint64_t		bytes_pending;
	boolean_t		terminating;
} uniqueue_t;

typedef struct zstream_team {
	uniqueue_t		queue;
	int			num_threads;
	thrd_t			*threads;
	perform_work_t		*process;
	uint64_t		batch_size;
	uint64_t		queue_length;
	boolean_t		finalized;
} zstream_team_t;


/*
 * Claim up to max_units work items, trying to accumulate at least
 * batch_size worth of payload data. Does not block waiting to reach
 * batch_size; returns whatever is available or awaits a condition if
 * nothing is available.
 *
 * If the first-available item does not require processing, scrape up all
 * the leading items of this type and return. The worker will end up
 * doing no work and coming right back here, but it simplifies the
 * code to handle this case through the normal process.
 */
static int
claim_batch(zstream_team_t *team, queue_slot_t **slots, int max_units) {

	uniqueue_t	*q = &team->queue;
	uint64_t 	bytes_claimed = 0;
	uint64_t 	count = 0;
	uint64_t	target_bytes = team->batch_size;
	boolean_t	junk_batch = B_FALSE;

	mtx_lock(&q->mutex);
	while (true) {
		if (q->claim >= q->insert) {
			cnd_wait(&q->inserted, &q->mutex);
			if (q->terminating) {
				mtx_unlock(&q->mutex);
				thrd_exit(0);
			}			
		}
		uint64_t fair_share = q->bytes_pending / team->num_threads;
		if (fair_share > team->batch_size && team->batch_size) {
			target_bytes = fair_share;
		}
		while (q->claim < q->insert && count < max_units &&
			bytes_claimed <= target_bytes)
		{
			uint64_t slot = q->claim % team->queue_length;
			if (!count && !q->slots[slot].unit.needs_processing) {
				junk_batch = B_TRUE;
			} else if (junk_batch && q->slots[slot].unit.needs_processing) {
				break;
			}
			slots[count] = &q->slots[slot];
			bytes_claimed += q->slots[slot].unit.payload_size;
			q->claim++;
			count++;
		}
		if (count) {
			mtx_unlock(&q->mutex);
			return count;			
		}
	}
}

static int
worker_thread(void *team_in)
{
	zstream_team_t 	*team = (zstream_team_t *)team_in;
	queue_slot_t	*batch[MAX_BATCH];
	uniqueue_t		*q = &team->queue;
	uint64_t		count = 0;

	while (true) {
		count = claim_batch(team, batch, MAX_BATCH);
		/* Complete the whole batch before returning any items */
		for (int i = 0; i < count; i++) {
			work_unit_t *unit = &batch[i]->unit;
			if (unit->needs_processing) {
				team->process(unit);
			}
		}
		mtx_lock(&q->mutex);
		for (int i = 0; i < count; i++) {
			batch[i]->completed = B_TRUE;
		}
		boolean_t any_completed = B_FALSE;
		while (q->slots[q->complete].completed) {
			q->complete++;
			any_completed = B_TRUE;
		}
		if (any_completed) {
			cnd_signal(&q->completed);
		}
		mtx_unlock(&q->mutex);
	}
	return 0;
}

void *
zstream_team_init(perform_work_t perform_work, uint64_t batch_size,
	uint64_t queue_length)
{
	zstream_team_t *team = safe_malloc(sizeof(zstream_team_t));

	memset(team, 0, sizeof (zstream_team_t));

	team->process = perform_work;
	team->batch_size = batch_size;
	team->queue_length = queue_length;
	team->queue.slots = safe_calloc(sizeof(queue_slot_t) * queue_length);

	mtx_init(&team->queue.mutex, mtx_plain);
	cnd_init(&team->queue.inserted);
	cnd_init(&team->queue.completed);
	cnd_init(&team->queue.dequeued);

	team->num_threads = sysconf(_SC_NPROCESSORS_ONLN);
	if (team->num_threads < MIN_THREADS) {
		team->num_threads = MIN_THREADS;
	}

	team->threads = safe_calloc(team->num_threads * sizeof(thrd_t));

	for (int i = 0; i < team->num_threads; i++) {
		if (thrd_create(&team->threads[i], worker_thread, team)
			!= thrd_success)
		{
			fprintf(stderr, "Error creating worker thread %d\n", i);
			exit(1);
		}
	}

	return (void *)team;
}

void
zstream_team_enqueue(void *team_in, work_unit_t *unit) {
	zstream_team_enqueue_impl(team_in, unit, B_FALSE);
}

static void
zstream_team_enqueue_impl(void *team_in, work_unit_t *unit, boolean_t last_one)
{
	zstream_team_t *team = (zstream_team_t *)team_in;
	uniqueue_t *q = &team->queue;

	mtx_lock(&q->mutex);
	while (q->insert - q->dequeue >= team->queue_length) {
		cnd_wait(&q->dequeued, &q->mutex);
		if (q->terminating) {
			mtx_unlock(&q->mutex);
			thrd_exit(0);
		}
	}
	uint64_t slot = q->insert % team->queue_length;
	q->slots[slot].unit = *unit;
	q->slots[slot].completed = B_FALSE;
	q->slots[slot].end_of_stream = last_one;
	if (unit->needs_processing) {
		q->bytes_pending += unit->payload_size;
	}
	q->insert++;
	team->finalized = last_one;
	cnd_signal(&q->inserted);
	mtx_unlock(&q->mutex);
}

int
zstream_team_dequeue(void *team_in, work_unit_t *unit) {
	zstream_team_t *team = (zstream_team_t *)team_in;
	uniqueue_t *q = &team->queue;
	mtx_lock(&q->mutex);
	while (true) {
		if (q->dequeue < q->complete) {
			uint64_t slot = q->dequeue % team->queue_length;
			q->dequeue++;
			if (q->slots[slot].end_of_stream) {
				mtx_unlock(&q->mutex);
				zstream_team_destroy(team);
				return (1);
			} else {
				*unit = q->slots[slot].unit;
				cnd_signal(&q->dequeued);
				mtx_unlock(&q->mutex);
				return (0);
			}
		}
		cnd_wait(&q->completed, &q->mutex);
		if (q->terminating) {
			mtx_unlock(&q->mutex);
			thrd_exit(0);
		}
	}
}

void
zstream_team_fini(void *team_in) {
	work_unit_t unit;
	memset(&unit, 0, sizeof(unit));
	zstream_team_enqueue_impl(team_in, &unit, B_TRUE);
}

void
zstream_team_destroy(zstream_team_t *team) {
	team->queue.terminating = B_TRUE;
	mtx_lock(&team->queue.mutex);
	cnd_broadcast(&team->queue.inserted);
	cnd_broadcast(&team->queue.completed);
	cnd_broadcast(&team->queue.dequeued);
	mtx_unlock(&team->queue.mutex);
	for (int i = 0; i < team->num_threads; i++) {
		thrd_join(team->threads[i], NULL);
	}
	mtx_destroy(&team->queue.mutex);
	cnd_destroy(&team->queue.inserted);
	cnd_destroy(&team->queue.completed);
	cnd_destroy(&team->queue.dequeued);
	free(team->threads);
	free(team->queue.slots);
}
