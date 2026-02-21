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

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#include "zstream_queue.h"
#include "zstream_shared.h"

#define MIN_THREADS 	6
#define MAX_QUEUES 	16	/* Greatest # of queues simultaneously active */
#define CLAIM_FACTOR	8	/* Weight used in thread-to-queue allocation */

/*
 * A zstream_queue is a ring buffer with four pointers: enqueue, claim,
 * complete, and dequeue, in that order. No pointer can move beyond its
 * preceding pointer. Every interval between pointers corresponds to a
 * specific state that applies to the intervening work items: enqueued,
 * claimed for work, completed. Since items never leave the ring buffer,
 * FIFO order is guaranteed for operations.
 *
 * No "claimed" condition is necessary because the next state in line is
 * "completed". That transition is where the actual work is done, so the
 * clock on that state is "when the work is done" rather than a gating
 * condition. In addition, the enqueued condition is at the level of the
 * thread pool because worker threads wait on it for incoming work while not
 * bound to any particular queue.
 */

typedef struct {
	queue_item	*qs_item;
	int		qs_cost;
	boolean_t	qs_completed;
	boolean_t	qs_end_of_stream;
} queue_slot_t;

struct zstream_queue {
	pthread_mutex_t		zq_mutex;
	pthread_cond_t		zq_completed, zq_dequeued;
	uint64_t		zq_enqueue, zq_claim, zq_complete, zq_dequeue;
	queue_slot_t		*zq_slots;
	int			zq_num_slots;
	size_t			zq_item_size;
	zq_process_item_f	*zq_process;
	zq_estimate_cost_f	*zq_cost;
	int			zq_batch_budget;
	boolean_t		zq_finalized;
};

typedef struct {
	pthread_mutex_t		tp_mutex;
	pthread_cond_t		tp_enqueued;
	zstream_queue_t		tp_queues[MAX_QUEUES];
	int			tp_num_queues;
	pthread_t		*tp_threads;
	int			tp_num_threads;
} thread_pool_t;

typedef void pthread_cleanup_f(void *);

static thread_pool_t pool = {};
static pthread_once_t once_control = PTHREAD_ONCE_INIT;

static void *queue_worker(void *);

static void
thread_pool_init(void) {
	pthread_mutex_init(&pool.tp_mutex, NULL);
	pthread_cond_init(&pool.tp_enqueued, NULL);
	srand48(0x83b606fb0dba7627);
}

/*
 * Must be called by a function holding the pool mutex
 *
 * sched_affinity() is a better estimate of available threads than
 * sysconf because sysconf doesn't take account of limits that might be
 * set on, e.g., a container.
 */
static void
thread_pool_spinup(void) {
	pool.tp_num_queues = 0;
#ifdef CPU_COUNT
	cpu_set_t cpu_set;
	sched_getaffinity(0, sizeof(cpu_set_t), &cpu_set);
	pool.tp_num_threads = CPU_COUNT(&cpu_set);
#else
	pool.tp_num_threads = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	if (pool.tp_num_threads < MIN_THREADS) {
		pool.tp_num_threads = MIN_THREADS;
	}
	pool.tp_threads = safe_malloc(sizeof(pthread_t) * pool.tp_num_threads);
	for (int i = 0; i < pool.tp_num_threads; i++) {
		pthread_create(&pool.tp_threads[i], NULL, queue_worker, NULL);
		pthread_detach(pool.tp_threads[i]);
	}
}

/*
 * Must be called by a function holding the pool mutex
 */
static void
thread_pool_spindown(void) {
	for (int i = 0; i < pool.tp_num_threads; i++) {
		pthread_cancel(pool.tp_threads[i]);
	}
	free(pool.tp_threads);
	pool.tp_threads = NULL;
	pool.tp_num_threads = 0;
}

static void
auto_unlock_mutex(pthread_mutex_t *mutex) {
	pthread_mutex_unlock(mutex);
}

static void
await_condition(pthread_cond_t *cond, pthread_mutex_t *mutex) {
	pthread_cleanup_push((pthread_cleanup_f *)auto_unlock_mutex, mutex);
	pthread_cond_wait(cond, mutex);
	pthread_cleanup_pop(0);
}

zstream_queue_t
zstream_queue_create(zq_params_t *params)
{
	pthread_once(&once_control, thread_pool_init);
	pthread_mutex_lock(&pool.tp_mutex);
	if (!pool.tp_num_threads) {
		thread_pool_spinup();
	}
	zstream_queue_t queue = safe_malloc(sizeof(struct zstream_queue));
	pool.tp_queues[pool.tp_num_queues] = queue;
	pool.tp_num_queues++;

	*queue = (struct zstream_queue) {
		.zq_num_slots = params->qp_queue_length,
		.zq_item_size = params->qp_item_size,
		.zq_process = params->qp_process,
		.zq_cost = params->qp_estimate_cost,
		.zq_batch_budget = params->qp_batch_budget,
		.zq_slots = safe_calloc(params->qp_queue_length *
			(sizeof(queue_slot_t) + params->qp_item_size)),
	};
	void *items_base = &queue->zq_slots[params->qp_queue_length];
	for (int i = 0; i < params->qp_queue_length; i++) {
		queue->zq_slots[i].qs_item = items_base + i * params->qp_item_size;
	}

	pthread_mutex_init(&queue->zq_mutex, NULL);
	pthread_cond_init(&queue->zq_completed, NULL);
	pthread_cond_init(&queue->zq_dequeued, NULL);

	pthread_mutex_unlock(&pool.tp_mutex);
	return queue;
}

/*
 * Calling thread must hold the queue lock
 */
static void
advance_completion_pointer(zstream_queue_t queue) {
	boolean_t any_completed = B_FALSE;
	while (queue->zq_complete < queue->zq_claim &&
		queue->zq_slots[queue->zq_complete % queue->zq_num_slots].qs_completed)
	{
		queue->zq_complete++;
		any_completed = B_TRUE;
	}
	if (any_completed) {
		pthread_cond_signal(&queue->zq_completed);
	}
}

/*
 * Identify the queue most in need of a worker thread and claim up to MAX_BATCH
 * work items, trying to accumulate at least queue->batch_budget worth of work
 * data (== "cost"). All items in a batch will be drawn from the same queue.
 *
 * Does not block waiting to reach batch_budget; returns whatever is available
 * or awaits the any_queue_enqueued condition if nothing is available.
 *
 * If the first-available item does not require processing, scrape up all the
 * leading items of this type and return. The worker will end up doing no work
 * and coming right back here, but it simplifies the code to handle this case
 * through the normal process. We cannot just mark those items as completed
 * on entry to the queue because
 */
static int
claim_batch(zstream_queue_t queue, queue_slot_t **batch)
{
	uint64_t 	cost_claimed = 0;
	uint64_t 	count = 0;

	pthread_mutex_lock(&queue->zq_mutex);

	while (queue->zq_claim < queue->zq_enqueue &&
		count < MAX_BATCH &&
		((!queue->zq_batch_budget && !count) ||
		    	cost_claimed < queue->zq_batch_budget))
	{
		uint64_t slot_num = queue->zq_claim % queue->zq_num_slots;
		queue_slot_t *slot = &queue->zq_slots[slot_num];
		if (slot->qs_cost == 0) {
			slot->qs_completed = B_TRUE;
		} else {
			cost_claimed += slot->qs_cost;
			batch[count] = slot;
			count++;
		}
		queue->zq_claim++;
	}

	advance_completion_pointer(queue);
	pthread_mutex_unlock(&queue->zq_mutex);
	return count;
}

/*
 * Worker threads are assigned to queues on each loop so that they can be
 * shifted dynamically to follow work. Queue assignments are serialized through
 * the pool mutex. If there appears to be no work available, threads sleep on
 * the pool-level enqueue condition. That condition is paired with the pool
 * mutex, which guarantees that enqueue signals will not arrive while a thread
 * is attempting to pick a queue. See note in zstream_enqueue_impl().
 */
static zstream_queue_t
assign_thread_to_queue(void)
{
	pthread_mutex_lock(&pool.tp_mutex);
	double scale, cum_weights[MAX_QUEUES];

start:	if (pool.tp_num_queues) {
		for (int i = 0; i < pool.tp_num_queues; i++) {
			zstream_queue_t queue = pool.tp_queues[i];
			int claimable_est = queue->zq_enqueue - queue->zq_claim;
			int in_queue_est = queue->zq_enqueue - queue->zq_dequeue;
			int open_slots_est = queue->zq_num_slots - in_queue_est;
			double claim_factor = (double)claimable_est / CLAIM_FACTOR;
			double slot_factor = (open_slots_est > 0) ?
				(1.0 / open_slots_est) : 2.0;
			double weight = fmin(1.0, slot_factor) * claim_factor;
			cum_weights[i] = weight + ((i == 0) ? 0.0 : cum_weights[i-1]);
		}
		scale = cum_weights[pool.tp_num_queues - 1];
	}
	if (scale < 0.0001) {
		await_condition(&pool.tp_enqueued, &pool.tp_mutex);
		goto start;
	} else {
		double select_val = drand48() * scale;
		for (int i = 0; i < pool.tp_num_queues; i++) {
			if (select_val <= cum_weights[i]) {
				zstream_queue_t selected_queue = pool.tp_queues[i];
				pthread_mutex_unlock(&pool.tp_mutex);
				return selected_queue;
			}
			abort();
		}
	}
}

static void *
queue_worker(void *dummy)
{
	(void) dummy;
	while (B_TRUE) {
		zstream_queue_t queue = assign_thread_to_queue();
		queue_slot_t *batch[MAX_BATCH];
		uint64_t count = claim_batch(queue, batch);
		/* Complete the whole batch before returning any items */
		for (int i = 0; i < count; i++) {
			queue->zq_process(batch[i]->qs_item);
		}
		pthread_mutex_lock(&queue->zq_mutex);
		for (int i = 0; i < count; i++) {
			batch[i]->qs_completed = B_TRUE;
		}
		advance_completion_pointer(queue);
		pthread_mutex_unlock(&queue->zq_mutex);
	}
	return NULL;
}

static void
zstream_enqueue_impl(zstream_queue_t queue, queue_item *item, boolean_t last_one)
{
	pthread_mutex_lock(&queue->zq_mutex);
	if (queue->zq_finalized) {
		pthread_mutex_unlock(&queue->zq_mutex);
		return;
	}
	while (queue->zq_enqueue - queue->zq_dequeue >= queue->zq_num_slots) {
		await_condition(&queue->zq_dequeued, &queue->zq_mutex);
	}
	queue_slot_t *slot = &queue->zq_slots[queue->zq_enqueue % queue->zq_num_slots];
	slot->qs_cost = queue->zq_cost(item);
	slot->qs_completed = B_FALSE;
	slot->qs_end_of_stream = last_one;
	memcpy(slot->qs_item, item, queue->zq_item_size);
	queue->zq_finalized = queue->zq_finalized || last_one;
	queue->zq_enqueue++;
	pthread_mutex_unlock(&queue->zq_mutex);

	/*
	 * Enqueueing doesn't require any participation from the thread pool.
	 * However, assign_thread_to_queue is a multistep calculation that is
	 * subject to skew among queues. It's possible for that function to conclude
	 * that no work is available in any queue even though something has been
	 * recently enqueued. If no one is listening when the enqueue signal is
	 * sent, the signal can potentially be dropped. So, the sender must ensure
	 * that no thread is in the queue assignment step when the signal is sent.
	 */
	pthread_mutex_lock(&pool.tp_mutex);
	pthread_cond_signal(&pool.tp_enqueued);
	pthread_mutex_unlock(&pool.tp_mutex);
}

void
zstream_enqueue(zstream_queue_t queue, queue_item *item) {
	zstream_enqueue_impl(queue, item, B_FALSE);
}

void
zstream_queue_fini(zstream_queue_t queue) {
	uint8_t item[queue->zq_item_size];
	zstream_enqueue_impl(queue, &item, B_TRUE);
}

/*
 * Must be called with the pool mutex held. Releases the mutex.
 */
static void
zstream_queue_destroy(zstream_queue_t queue) {
	pthread_mutex_destroy(&queue->zq_mutex);
	pthread_cond_destroy(&queue->zq_completed);
	pthread_cond_destroy(&queue->zq_dequeued);
	free(queue->zq_slots);
	queue->zq_slots = NULL;
	free(queue);
	if (pool.tp_num_queues == 1) {
		thread_pool_spindown();
	} else {
		for (int i = 0; i < pool.tp_num_queues; i++) {
			if (queue == pool.tp_queues[i]) {
				memmove(&pool.tp_queues[i], &pool.tp_queues[i+1],
					(pool.tp_num_queues - i - 1) * sizeof(zstream_queue_t));
				break;
			}
		}
	}
	pool.tp_num_queues--;
	pthread_mutex_unlock(&pool.tp_mutex);
}

boolean_t
zstream_dequeue(zstream_queue_t queue, queue_item *item) {
	pthread_mutex_lock(&queue->zq_mutex);
	while (queue->zq_dequeue >= queue->zq_complete) {
		await_condition(&queue->zq_completed, &queue->zq_mutex);
	}
	uint64_t slot = queue->zq_dequeue % queue->zq_num_slots;
	queue->zq_dequeue++;
	if (queue->zq_slots[slot].qs_end_of_stream) {
		zstream_queue_destroy(queue);
		return B_FALSE;
	} else {
		memcpy(item, queue->zq_slots[slot].qs_item, queue->zq_item_size);
		pthread_mutex_unlock(&queue->zq_mutex);
		pthread_cond_signal(&queue->zq_dequeued);
		return B_TRUE;
	}
}

