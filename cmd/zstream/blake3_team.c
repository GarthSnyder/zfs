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
#include <sys/blake3.h>
#include "blake3_team.h"
#include "zstream_shared.h"

#define MIN_THREADS 6
#define QUEUE_SIZE B3_TEAM_QUEUE_SIZE


/*
 * Claim up to max_units work items, trying to accumulate at least
 * min_bytes worth of payload data. Does not block waiting to reach
 * min_bytes; returns whatever is available or awaits a condition if
 * nothing is available.
 *
 * If the first-available item does not require hashing, scrape up all
 * the leading items of this type and return. The worker will end up
 * doing no work and coming right back here, but it simplifies the 
 * code to handle this case through the normal process.
 */
static int
claim_batch(blake3_team_t *team, b3_work_unit_t **units, int max_units) {

	b3_uniqueue_t	*q = &team->queue;
	uint64_t 		bytes_claimed = 0;
	uint64_t 		count = 0;
	uint64_t		target_bytes = B3_TEAM_MIN_BATCH_SIZE;
	boolean_t		junk_load = B_FALSE;

	mtx_lock(&q->mutex);
	while (true) {
		uint64_t fair_share = q->bytes_pending / team->num_threads;
		if (fair_share > B3_TEAM_MIN_BATCH_SIZE) {
			target_bytes = fair_share;
		}
		/* Take all the no-work entries */
		while (q->claim < q->insert && count < max_units 
			&& bytes_claimed < target_bytes) 
		{
			if (junk_load && q->slots[q->claim % QUEUE_SIZE].needs_blake3) {
				break;
			}
			units[count] = &q->slots[q->claim % QUEUE_SIZE];
			q->claim++;
			if (units[count].needs_blake3) {
				bytes_claimed += units[count].payload_size;
				q->bytes_pending -= units[count].payload_size;
			} else {
				junk_load = B_TRUE;
			}
			count++;
		}
		if (count) {
			cnd_signal(&q->claimed);
			mtx_unlock(&q->mutex);
			return count;
		} else {
		cnd_wait(&q->inserted);
		if (q->terminate) {
			thrd_exit(0);
		}
	}
}

static int
worker_thread(void *team)
{
	blake3_team_t 		*team = (blake3_team_t *)team_in;
	drr_work_unit_t		*batch[QUEUE_SIZE];
	b3_uniqueue_t		*q = team->queue;
	uint64_t			count = 0;

	while (true) {
		count = claim_batch(team, b3_work_unit_t **units, int max_units);
		/* Complete the whole batch before returning any items */
		for (int i = 0; i < count; i++) {
			drr_work_unit_t *unit = batch[i];
			if (unit->payload != NULL && unit->payload_size > 0
				&& unit->needs_blake3)
			{
				BLAKE3_CTX ctx;
				Blake3_Init(&ctx);
				Blake3_Update(&ctx, unit->payload, unit->payload_size);
				Blake3_Final(&ctx, (uint8_t *)&unit->blake3_cksum);
			}
			unit->completed = B_TRUE;
		}
		mtx_lock(&q->mutex);
		boolean_t any_completed = B_FALSE;
		while (q->slots[q->complete].completed) {
			q->complete++;
			any_completed = B_TRUE;
		}
		cnd_signal(&q->completed);
		mtx_unlock(&q->mutex);
	}
	return 0;
}

void
blake3_team_init(blake3_team_t *team)
{
	if (team->initialized)
		return;

	memset(team, 0, sizeof (blake3_team_t));

	cnd_init(&team->queue.inserted);
	cnd_init(&team->queue.claimed);
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

	team->initialized = B_TRUE;
}

static void
blake3_team_enqueue(blake3_team_t *team, struct drr_work_unit *unit)
{
	b3_uniqueue_t *q = &team->queue;

	mtx_lock(&q->mutex);
	while (q->insert - q.dequeue >= QUEUE_SIZE) {
		cnd_wait(&q->dequeued);
		if (q->terminate) {
			thrd_exit(0);
		}
	}
	q->slots[q->insert % QUEUE_SIZE] = *unit;
	q->insert++;
	if (unit->needs_blake3) {
		q->bytes_pending += unit->payload_size;
	}
	cnd_signal(&q->dequeued);
	mtx_unlock(&q->mutex);
}

static void
blake3_team_dequeue(blake3_team_t * team, drr_work_unit_t *unit) {
	b3_uniqueue_t *q = &team->queue;
	assert(team->initialized);
	mtx_lock(&q->mutex);
	while (true) {
		if (q->dequeue < q->complete) {
			*unit = q->slots[q->dequeue % QUEUE_SIZE];
			q->dequeue++;
			cnd_signal(&q->dequeued);
			mtx_unlock(&q->mutex);
			return;
		}
		cnd_wait(&q->completed);
		if (q->terminate) {
			thrd_exit(0);
		}
	}
}

void
blake3_team_destroy(blake3_team_t *team) {
	assert(team->initialized);
	team->queue.terminate = B_TRUE;
	cnd_broadcast(&team->queue.inserted);
	cnd_broadcast(&team->queue.claimed);
	cnd_broadcast(&team->queue.completed);
	cnd_broadcast(&team->queue.dequeued);
	for (i = 0; i < team->num_threads; i++) {
		thrd_join(team->threads[i], NULL);
	}
	mtx_destroy(&team->queue.mutex);
	cnd_destroy(&team->queue.inserted);
	cnd_destroy(&team->queue.claimed);
	cnd_destroy(&team->queue.completed);
	cnd_destroy(&team->queue.dequeued);
}