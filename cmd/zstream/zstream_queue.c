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
#include <pthreads.h>
#include <sched.h>
#include "team.h"
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
} queue_slot;

struct zstream_queue {
	pthread_mutex_t		mutex;
	queue_slot			*slots;
	int					num_slots;
	process_item_func	*process;
	int					batch_budget;
	int					thread_limit;
	int					threads_assigned;
	uint64_t			enqueue, claim, complete, dequeue;
	pthread_cond_t		completed, dequeued;
	uint64_t			bytes_pending;
	boolean_t			finalized;
};

typedef struct {
	pthread_mutex_t		mutex;
	pthread_cond_t		enqueued;
	zstream_queue		queues[MAX_QUEUES];
	int					num_queues;
	pthread_t			*threads;
	int					num_threads;
} thread_pool;

static thread_pool		pool = {};

static pthread_once_t once_control = PTHREAD_ONCE_INIT;

static void
thread_pool_init(void) {
	pthread_mutex_init(&pool.mutex, NULL);
	pthread_cond_init(&pool, NULL);
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
		pthread_create(&pool.threads[i], NULL, worker_thread, NULL);
	}
}

/*
 * Must be called by a function holding the pool mutex
 */
static void
thread_pool_spindown(void) {
	pool.num_queues = 0;
	for (int i = 0; i < pool.num_threads; i++) {
		pthread_cancel(pool.threads[i]);
		assert(pthread_join(pool.threads[i], NULL) == 0);
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
	pthread_cleanup_push(unlock_mutex, mutex);
	pthread_cond_wait(cond, mutex);
	pthread_cleanup_pop(0);
}

zstream_queue
zstream_queue_create(process_item_func *process, size_t item_size,
	int batch_budget, uint64_t queue_length, int max_threads);
{
	pthread_once(&once_control, thread_pool_init);
	pthread_mutex_lock(&pool.mutex);
	if (pool.num_threads) {
		thread_pool_spinup();
	}
	zstream_queue queue = safe_malloc(sizeof(struct zstream_queue));
	pool.queues[pool.num_queues++] = queue;
	pool.num_queues++;

	*queue = (struct zstream_queue) {
		.num_slots = queue_length,
		.process = process,
		.batch_budget = batch_budget,
		.num_slots = queue_length,
		.slots = safe_calloc(sizeof(queue_slot) * queue_length),
		.thread_limit = max_threads,
	}

	pthread_mutex_init(&queue->mutex, NULL);
	pthread_cond_init(&queue->inserted);
	pthread_cond_init(&queue->completed);
	pthread_cond_init(&queue->dequeued);

	pthread_mutex_unlock(&pool.mutex);
	return queue;
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
claim_batch(zstream_queue queue, queue_item **batch)
{
	uint64_t 	cost_claimed = 0;
	uint64_t 	count = 0;
	boolean_t	any_completed = B_FALSE;

	pthread_mutex_lock(&queue->mutex);

	while (q->claim < q->insert &&
		count < MAX_BATCH &&
		cost_claimed <= queue->batch_budget)
	{
		uint64_t slot_num = queue->claim % queue->queue_length;
		queue_slot *slot = &queue->slots[slot_num];
		if (slot->cost == 0) {
			slot->completed = B_TRUE;
		} else {
			cost_claimed += slot->cost;
			slots[count] = slot;
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
zstream_queue
assign_thread_to_queue(void) {
	pid_t my_tid = gettid();
	static unsigned short xsubi[3] = {my_tid, my_tid, my_tid};
	pthread_mutex_lock(&pool.mutex);
	while (true) {
		double cum_weights[MAX_QUEUES];
		for (int i = 0; i < pool.num_queues; i++) {
			queue = pool.queues[i];
			int claimable_est = queue->enqueue - queue->claim;
			int in_queue_est = queue->enqueue - queue->dequeue;
			int open_slots_est = queue->queue_length - in_queue_est;
			double claim_factor = claimable_est / CLAIM_FACTOR;
			double slot_factor = (open_slots_est > 0) ?
				(1.0 / open_slots_est) : 2.0;
			double weight = fmin(1.0, slot_factor) * claim_factor;
			cum_weights[i] = weight + ((i == 0) ? 0.0 : cum_weights[i-1]);
		}
		double scale = cum_weights[pool.num_queues - 1];
		if (scale < 0.01) {
			pthread_mutex_unlock(&pool.mutex);
			pthread_cond_wait(&pool.enqueued, &pool.mutex);
			continue;
		} else {
			double select_val = erand48(xsubi) * cum_weight;
			for (int i = 0; i < pool.num_queues; i++) {
				if (cum_weights[i] <= select_val) {
					zstream_queue selected_queue = pool.queues[i];
					pthread_mutex_unlock(&pool.mutex);
					return selected_queue;
				}
			}
		}
	}
}

static void
advance_completion_pointer(zstream_queue queue) {
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

static void *
worker_thread(void *dummy)
{
	while (true) {
		zstream_queue queue = assign_thread_to_queue();
		queue_item *batch[MAX_BATCH];
		uint64_t count = claim_batch(queue, batch);
		/* Complete the whole batch before returning any items */
		for (int i = 0; i < count; i++) {
			queue->process(&batch[i]->item);
		}
		pthread_mutex_lock(&queue->mutex);
		for (int i = 0; i < count; i++) {
			&batch[i]->item->completed = B_TRUE;
		}
		advance_completion_pointer(queue);
		pthread_mutex_unlock(&queue->mutex);
	}
}

static void
zstream_enqueue_impl(zstream_queue queue, queue_item *item, int cost,
	boolean_t last_one)
{
	pthread_mutex_lock(&queue->mutex);
	if (queue->finalized) {
		pthread_mutex_unlock(&queue->mutex);
		return;
	}
	while (queue->insert - queue->dequeue >= team->queue_length) {
		await_condition(&queue->dequeued, &queue->mutex);
	}
	uint64_t slot = queue->insert % team->queue_length;
	queue->slots[slot] = (queue_slot) {
		.item = item,
		.cost = cost,
		.end_of_stream = last_one
	};
	queue->bytes_pending += cost;
	queue->finalized = queue->finalized || last_one;
	queue->insert++;
	pthread_mutex_unlock(&queue->mutex);
	pthread_cond_signal(&queue->enqueued);

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
	pthread_cond_signal(&pool.any_queue_enqueued)
	pthread_mutex_unlock(&pool.mutex);
}

void
zstream_enqueue(zstream_queue queue, queue_item *item, int cost) {
	zstream_enqueue_impl(queue, item, cost, B_FALSE);
}

void
zstream_queue_fini(zstream_queue queue) {
	struct queue_item item = {};
	team_enqueue_impl(queue, &item, 0, B_TRUE);
}

boolean_t
zstream_dequeue(zstream_queue queue, queue_item *item); {
	pthread_mutex_lock(&queue->mutex);
	while (queue->dequeue >= queue->complete) {
		await_condition(&queue->completed, &queue->mutex);
	}
	uint64_t slot = queue->dequeue % team->queue_length;
	queue->dequeue++;
	if (queue->slots[slot].end_of_stream) {
		pthread_mutex_unlock(&queue->mutex);
		zstream_queue_destroy(queue);
		return B_FALSE;
	} else {
		*item = *(queue->slots[slot].item);
		pthread_mutex_unlock(&queue->mutex);
		pthread_cond_signal(&q->dequeued);
		return B_TRUE;
	}
}

static void
zstream_queue_destroy(zstream_queue queue) {
	pthread_mutex_lock(&queue->mutex);
	/*
	 * All items have been processed, so this should always be true.
	 * Furthermore, there should be no possibility of new threads
	 * attempting to enter the queue.
	 */
	assert(!queue->threads_assigned);
	pthread_mutex_unlock(&queue->mutex);
	pthread_mutex_lock(&pool->mutex);
	pthread_mutex_destroy(&queue->queue.mutex);
	pthread_cond_destroy(&queue->inserted);
	pthread_cond_destroy(&queue->completed);
	pthread_cond_destroy(&queue->dequeued);
	free(queue->slots);
	free(queue);
	if (pool.num_queues == 1) {
		thread_pool_spindown();
	} else {
		for (int i = 0; i < pool.num_queues; i++) {
			if (queue == &pool.queues[i]) {
				memmove(&pool.queues[i], &pool.queues[i+1],
					(pool.num_queues - i - 1) * sizeof(zstream_queue));
				break;
			}
		}
	}
	pool.num_queues--;
	pthread_mutex_unlock(&pool.mutex);
}

