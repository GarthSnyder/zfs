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

/*
 * Detect the number of physical CPU cores.
 * On Linux, parse /proc/cpuinfo to count unique physical cores.
 * On other systems, use a heuristic based on logical processors.
 */
static int
get_physical_cores(void)
{
	int cores = 0;

#ifdef __linux__
	FILE *fp = fopen("/proc/cpuinfo", "r");
	if (fp != NULL) {
		char line[256];
		int max_core_id = -1;
		int max_physical_id = -1;
		int core_id = -1;
		int physical_id = -1;
		bool seen_processor = false;

		while (fgets(line, sizeof (line), fp) != NULL) {
			if (strncmp(line, "processor", 9) == 0) {
				/*
				 * Save previous processor's IDs if we have them
				 */
				if (seen_processor) {
					if (core_id > max_core_id)
						max_core_id = core_id;
					if (physical_id > max_physical_id)
						max_physical_id = physical_id;
				}
				seen_processor = true;
				core_id = -1;
				physical_id = -1;
			} else if (strncmp(line, "core id", 7) == 0) {
				char *p = strchr(line, ':');
				if (p != NULL)
					core_id = atoi(p + 1);
			} else if (strncmp(line, "physical id", 11) == 0) {
				char *p = strchr(line, ':');
				if (p != NULL)
					physical_id = atoi(p + 1);
			}
		}

		/*
		 * Don't forget the last processor
		 */
		if (seen_processor) {
			if (core_id > max_core_id)
				max_core_id = core_id;
			if (physical_id > max_physical_id)
				max_physical_id = physical_id;
		}

		fclose(fp);

		/*
		 * Total cores = (max_physical_id + 1) * (max_core_id + 1)
		 * This assumes symmetric multi-processing.
		 */
		if (max_core_id >= 0 && max_physical_id >= 0) {
			cores = (max_physical_id + 1) * (max_core_id + 1);
		}
	}
#endif

	/*
	 * Fallback for non-Linux or if /proc/cpuinfo parsing failed
	 */
	if (cores == 0) {
		long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
		if (nprocs <= 0)
			nprocs = 4;

		/*
		 * Assume hyperthreading: divide by 2 to get physical cores
		 */
		cores = (int)(nprocs / 2);
	}

	/*
	 * Subtract 2 as requested, minimum of 1
	 */
	cores = cores - 2;
	if (cores < 1)
		cores = 1;

	return (cores);
}

static void
init_queue(work_queue_t *q)
{
	memset(q, 0, sizeof (*q));
	mtx_init(&q->mutex, mtx_plain);
	cnd_init(&q->not_full);
	cnd_init(&q->not_empty);
}

static void
enqueue(work_queue_t *q, struct drr_work_unit *unit)
{
	mtx_lock(&q->mutex);

	while (q->count == QUEUE_SIZE) {
		cnd_wait(&q->not_full, &q->mutex);
	}

	q->slots[q->tail] = unit;
	q->tail = (q->tail + 1) % QUEUE_SIZE;
	q->count++;
	q->total_payload_size += unit->payload_size;

	cnd_signal(&q->not_empty);
	mtx_unlock(&q->mutex);
}

/*
 * Dequeue up to max_units work items, trying to accumulate at least
 * min_bytes worth of payload data. Does not block waiting to reach
 * min_bytes; returns whatever is available.
 */
static int
dequeue_batch(work_queue_t *q, struct drr_work_unit **units,
    int max_units, uint64_t min_bytes)
{
	mtx_lock(&q->mutex);

	while (q->count == 0) {
		cnd_wait(&q->not_empty, &q->mutex);
	}

	int dequeued = 0;
	uint64_t bytes_claimed = 0;

	while (dequeued < max_units && q->count > 0) {
		units[dequeued] = q->slots[q->head];
		bytes_claimed += units[dequeued]->payload_size;
		q->head = (q->head + 1) % QUEUE_SIZE;
		q->count--;
		q->total_payload_size -= units[dequeued]->payload_size;
		dequeued++;

		/*
		 * If we've met the minimum and there's still work available,
		 * stop here to give other threads a chance.
		 */
		if (bytes_claimed >= min_bytes && q->count > 0) {
			break;
		}
	}

	cnd_signal(&q->not_full);
	mtx_unlock(&q->mutex);

	return (dequeued);
}

static int
worker_thread(void *arg)
{
	struct drr_work_unit *batch[QUEUE_SIZE];

	(void) arg;

	while (true) {
		uint64_t target_bytes = MIN_BATCH_SIZE;

		/*
		 * Calculate a fair share of available work.
		 * If the total payload size is large, each thread can
		 * claim up to (total_payload_size / num_threads).
		 */
		mtx_lock(&blake3_team.input_queue.mutex);
		if (blake3_team.input_queue.total_payload_size > 0) {
			uint64_t fair_share =
			    blake3_team.input_queue.total_payload_size /
			    blake3_team.num_threads;
			if (fair_share > target_bytes)
				target_bytes = fair_share;
		}
		mtx_unlock(&blake3_team.input_queue.mutex);

		int count = dequeue_batch(&blake3_team.input_queue, batch,
		    QUEUE_SIZE, target_bytes);

		/*
		 * Process each work unit in the batch
		 */
		for (int i = 0; i < count; i++) {
			struct drr_work_unit *unit = batch[i];

			/*
			 * Hash the payload if present
			 */
			if (unit->payload != NULL && unit->payload_size > 0) {
				BLAKE3_CTX ctx;
				Blake3_Init(&ctx);
				Blake3_Update(&ctx, unit->payload,
				    unit->payload_size);
				Blake3_Final(&ctx, (uint8_t *)&unit->blake3);
			}

			/*
			 * Place the completed unit in the output queue.
			 * The output queue is a circular buffer indexed by
			 * sequence_num % QUEUE_SIZE. We must wait if the
			 * slot is still occupied by a previous result.
			 */
			mtx_lock(&blake3_team.output_mutex);

			int slot_idx = unit->sequence_num % QUEUE_SIZE;
			while (blake3_team.output_slots[slot_idx].ready) {
				cnd_wait(&blake3_team.output_ready,
				    &blake3_team.output_mutex);
			}

			blake3_team.output_slots[slot_idx].unit = unit;
			blake3_team.output_slots[slot_idx].ready = true;

			cnd_broadcast(&blake3_team.output_ready);
			mtx_unlock(&blake3_team.output_mutex);
		}
	}

	return (0);
}

void
blake3_team_init(void)
{
	if (blake3_team.initialized)
		return;

	memset(&blake3_team, 0, sizeof (blake3_team));

	init_queue(&blake3_team.input_queue);

	mtx_init(&blake3_team.output_mutex, mtx_plain);
	cnd_init(&blake3_team.output_ready);

	blake3_team.num_threads = get_physical_cores();
	blake3_team.threads = safe_calloc(blake3_team.num_threads *
	    sizeof (thrd_t));

	for (int i = 0; i < blake3_team.num_threads; i++) {
		if (thrd_create(&blake3_team.threads[i], worker_thread,
		    NULL) != thrd_success) {
			fprintf(stderr, "Error creating worker thread %d\n", i);
			exit(1);
		}
	}

	blake3_team.initialized = true;
}

void
blake3_team_submit(struct drr_work_unit *unit)
{
	assert(blake3_team.initialized);
	enqueue(&blake3_team.input_queue, unit);
}

struct drr_work_unit *
blake3_team_retrieve(void)
{
	assert(blake3_team.initialized);

	mtx_lock(&blake3_team.output_mutex);

	int slot_idx = blake3_team.next_output_seq % QUEUE_SIZE;

	/*
	 * Wait for the next expected sequence number to be ready
	 */
	while (!blake3_team.output_slots[slot_idx].ready) {
		cnd_wait(&blake3_team.output_ready,
		    &blake3_team.output_mutex);
	}

	struct drr_work_unit *unit = blake3_team.output_slots[slot_idx].unit;
	blake3_team.output_slots[slot_idx].ready = false;
	blake3_team.output_slots[slot_idx].unit = NULL;
	blake3_team.next_output_seq++;

	cnd_broadcast(&blake3_team.output_ready);
	mtx_unlock(&blake3_team.output_mutex);

	return (unit);
}
