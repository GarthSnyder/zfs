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
 *
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
#include <pthread.h>
#include <sched.h>
#include <math.h>
#include "zstream_queue.h"
#include "zstream_shared.h"

#define MIN_THREADS 6
#define MAX_BATCH 16	/* Most items that can be claimed at once */
#define MAX_QUEUES 16	/* Greatest number of queues simultaneously active */
#define CLAIM_FACTOR 8	/* Used in calculating thread assignments */

/*
 * A zstream_queue is a ring buffer with four pointers: enqueue, claim,
 * complete, and dequeue, in that order. No pointer can move beyond its
 * preceding pointer. Every interval between pointers corresponds to a
 * specific state that applies to the intervening work items: enqueued,
 * claimed for work, completed. FIFO order is guaranteed everywhere.
 *
 * No "claimed" condition is necessary because the next state in line is
 * "completed". That transition is where the actual work is done, so the
 * clock on that state is "when the work is done" rather than a gating
 * condition. In addition, the enqueued condition is at the level of the
 * thread pool because worker threads wait on it for incoming work while
 * not bound to any particular queue.
 */

typedef struct {
	queue_item	*item;
	int			cost;
	boolean_t	completed;
	boolean_t	end_of_stream;
} queue_slot_t;

struct zstream_queue {
	pthread_mutex_t		mutex;
	pthread_cond_t		completed, dequeued;
	uint64_t			enqueue, claim, complete, dequeue;
	queue_slot_t		*slots;
	int					num_slots;
	size_t				item_size;
	zq_process_item_f	*process;
	zq_estimate_cost_f	*cost;
	int					batch_budget;
	boolean_t			finalized;
};

typedef struct {
	pthread_mutex_t		mutex;
	pthread_cond_t		enqueued;
	zstream_queue_t		queues[MAX_QUEUES];
	int					num_queues;
	pthread_t			*threads;
	int					num_threads;
} thread_pool_t;

typedef void pthread_cleanup_f(void *);

static thread_pool_t pool = {};
static pthread_once_t once_control = PTHREAD_ONCE_INIT;

static void *queue_worker(void *);

static void
thread_pool_init(void) {
	pthread_mutex_init(&pool.mutex, NULL);
	pthread_cond_init(&pool.enqueued, NULL);
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
	pool.num_queues = 0;
#ifdef CPU_COUNT
	cpu_set_t cpu_set;
	sched_getaffinity(0, sizeof(cpu_set_t), &cpu_set);
	pool.num_threads = CPU_COUNT(&cpu_set);
#else
	pool.num_threads = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	if (pool.num_threads < MIN_THREADS) {
		pool.num_threads = MIN_THREADS;
	}
	pool.threads = safe_malloc(sizeof(pthread_t) * pool.num_threads);
	for (int i = 0; i < pool.num_threads; i++) {
		pthread_create(&pool.threads[i], NULL, queue_worker, NULL);
		pthread_detach(pool.threads[i]);
	}
}

/*
 * Must be called by a function holding the pool mutex
 */
static void
thread_pool_spindown(void) {
	for (int i = 0; i < pool.num_threads; i++) {
		pthread_cancel(pool.threads[i]);
	}
	free(pool.threads);
	pool.threads = NULL;
	pool.num_threads = 0;
}

static void
unlock_mutex(pthread_mutex_t *mutex) {
	pthread_mutex_unlock(mutex);
}

static void
await_condition(pthread_cond_t *cond, pthread_mutex_t *mutex) {
	pthread_cleanup_push((pthread_cleanup_f *)unlock_mutex, mutex);
	pthread_cond_wait(cond, mutex);
	pthread_cleanup_pop(0);
}

zstream_queue_t
zstream_queue_create(zq_params_t *params)
{
	pthread_once(&once_control, thread_pool_init);
	pthread_mutex_lock(&pool.mutex);
	if (!pool.num_threads) {
		thread_pool_spinup();
	}
	zstream_queue_t queue = safe_malloc(sizeof(struct zstream_queue));
	pool.queues[pool.num_queues] = queue;
	pool.num_queues++;

	*queue = (struct zstream_queue) {
		.num_slots = params->qp_queue_length,
		.item_size = params->qp_item_size,
		.process = params->qp_process,
		.cost = params->qp_estimate_cost,
		.batch_budget = params->qp_batch_budget,
		.slots = safe_calloc(params->qp_queue_length *
			(sizeof(queue_slot_t) + params->qp_item_size)),
	};
	void *items_base = &queue->slots[params->qp_queue_length];
	for (int i = 0; i < params->qp_queue_length; i++) {
		queue->slots[i].item = items_base + i * params->qp_item_size;
	}

	pthread_mutex_init(&queue->mutex, NULL);
	pthread_cond_init(&queue->completed, NULL);
	pthread_cond_init(&queue->dequeued, NULL);

	pthread_mutex_unlock(&pool.mutex);
	return queue;
}

/*
 * Calling thread must hold the queue lock
 */
static void
advance_completion_pointer(zstream_queue_t queue) {
	boolean_t any_completed = B_FALSE;
	while (queue->complete < queue->claim &&
		queue->slots[queue->complete % queue->num_slots].completed)
	{
		queue->complete++;
		any_completed = B_TRUE;
	}
	if (any_completed) {
		pthread_cond_signal(&queue->completed);
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

	pthread_mutex_lock(&queue->mutex);

	while (queue->claim < queue->enqueue && count < MAX_BATCH &&
		(!queue->batch_budget || (cost_claimed < queue->batch_budget)))
	{
		uint64_t slot_num = queue->claim % queue->num_slots;
		queue_slot_t *slot = &queue->slots[slot_num];
		if (slot->cost == 0) {
			slot->completed = B_TRUE;
		} else {
			cost_claimed += slot->cost;
			batch[count] = slot;
			count++;
		}
		queue->claim++;
	}

	advance_completion_pointer(queue);
	pthread_mutex_unlock(&queue->mutex);
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
assign_thread_to_queue(void) {
	pthread_mutex_lock(&pool.mutex);
	while (true) {
		double scale = 0.0;
		double cum_weights[MAX_QUEUES];
		if (pool.num_queues) {
			for (int i = 0; i < pool.num_queues; i++) {
				zstream_queue_t queue = pool.queues[i];
				int claimable_est = queue->enqueue - queue->claim;
				int in_queue_est = queue->enqueue - queue->dequeue;
				int open_slots_est = queue->num_slots - in_queue_est;
				double claim_factor = (double)claimable_est / CLAIM_FACTOR;
				double slot_factor = (open_slots_est > 0) ?
					(1.0 / open_slots_est) : 2.0;
				double weight = fmin(1.0, slot_factor) * claim_factor;
				cum_weights[i] = weight + ((i == 0) ? 0.0 : cum_weights[i-1]);
			}
			scale = cum_weights[pool.num_queues - 1];
		}
		if (scale < 0.0001) {
			await_condition(&pool.enqueued, &pool.mutex);
			continue;
		} else {
			double select_val = drand48() * scale;
			for (int i = 0; i < pool.num_queues; i++) {
				if (select_val <= cum_weights[i]) {
					zstream_queue_t selected_queue = pool.queues[i];
					pthread_mutex_unlock(&pool.mutex);
					return selected_queue;
				}
			}
		}
	}
}

static void *
queue_worker(void *dummy)
{
	(void) dummy;
	while (true) {
		zstream_queue_t queue = assign_thread_to_queue();
		queue_slot_t *batch[MAX_BATCH];
		uint64_t count = claim_batch(queue, batch);
		/* Complete the whole batch before returning any items */
		for (int i = 0; i < count; i++) {
			queue->process(batch[i]->item);
		}
		pthread_mutex_lock(&queue->mutex);
		for (int i = 0; i < count; i++) {
			batch[i]->completed = B_TRUE;
		}
		advance_completion_pointer(queue);
		pthread_mutex_unlock(&queue->mutex);
	}
	return NULL;
}

static void
zstream_enqueue_impl(zstream_queue_t queue, queue_item *item, boolean_t last_one)
{
	pthread_mutex_lock(&queue->mutex);
	if (queue->finalized) {
		pthread_mutex_unlock(&queue->mutex);
		return;
	}
	while (queue->enqueue - queue->dequeue >= queue->num_slots) {
		await_condition(&queue->dequeued, &queue->mutex);
	}
	queue_slot_t *slot = &queue->slots[queue->enqueue % queue->num_slots];
	slot->cost = queue->cost(item);
	slot->completed = B_FALSE;
	slot->end_of_stream = last_one;
	memcpy(slot->item, item, queue->item_size);
	queue->finalized = queue->finalized || last_one;
	queue->enqueue++;
	pthread_mutex_unlock(&queue->mutex);

	/*
	 * Enqueueing doesn't require any participation from the thread pool.
	 * However, assign_thread_to_queue is a multistep calculation that is
	 * subject to skew among queues. It's possible for that function to conclude
	 * that no work is available in any queue even though something has been
	 * recently enqueued. If no one is listening when the enqueue signal is
	 * sent, the signal would be dropped. So, the sender must ensure that no
	 * thread is in the queue assignment step when the signal is sent.
	 */
	pthread_mutex_lock(&pool.mutex);
	pthread_cond_signal(&pool.enqueued);
	pthread_mutex_unlock(&pool.mutex);
}

void
zstream_enqueue(zstream_queue_t queue, queue_item *item) {
	zstream_enqueue_impl(queue, item, B_FALSE);
}

void
zstream_queue_fini(zstream_queue_t queue) {
	uint8_t item[queue->item_size];
	zstream_enqueue_impl(queue, &item, B_TRUE);
}

/*
 * Must be called with the pool mutex held. Releases the mutex.
 */
static void
zstream_queue_destroy(zstream_queue_t queue) {
	pthread_mutex_destroy(&queue->mutex);
	pthread_cond_destroy(&queue->completed);
	pthread_cond_destroy(&queue->dequeued);
	free(queue->slots);
	free(queue);
	if (pool.num_queues == 1) {
		thread_pool_spindown();
	} else {
		for (int i = 0; i < pool.num_queues; i++) {
			if (queue == pool.queues[i]) {
				memmove(&pool.queues[i], &pool.queues[i+1],
					(pool.num_queues - i - 1) * sizeof(zstream_queue_t));
				break;
			}
		}
	}
	pool.num_queues--;
	pthread_mutex_unlock(&pool.mutex);
}

boolean_t
zstream_dequeue(zstream_queue_t queue, queue_item *item) {
	pthread_mutex_lock(&queue->mutex);
	while (queue->dequeue >= queue->complete) {
		await_condition(&queue->completed, &queue->mutex);
	}
	uint64_t slot = queue->dequeue % queue->num_slots;
	queue->dequeue++;
	if (queue->slots[slot].end_of_stream) {
		zstream_queue_destroy(queue);
		return B_FALSE;
	} else {
		memcpy(item, queue->slots[slot].item, queue->item_size);
		pthread_mutex_unlock(&queue->mutex);
		pthread_cond_signal(&queue->dequeued);
		return B_TRUE;
	}
}

