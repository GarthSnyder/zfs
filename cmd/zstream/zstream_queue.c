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

#include <pthread.h>

#include "zstream_queue.h"
#include "zstream_shared.h"

#define MIN_THREADS 	6
#define MAX_QUEUES 	16	/* Greatest # of queues simultaneously active */
#define PLENTY_OF_WORK	6	/* Weight used in thread-to-queue allocation */
#define NO_WORK		0.0001	/* Score threshold for "no work" */

#define DEQUEUE_SCORE_WEIGHT 0.3	/* Relative weight of dequeue score */

/*
 * A zstream_queue is a ring buffer with four pointers: enqueue, claim,
 * complete, and dequeue, in that order. No pointer can move beyond its
 * preceding pointer. Every interval between pointers contains work items in
 * a particular state: enqueued, claimed for work, completed. Since items
 * never leave the ring buffer, FIFO order is guaranteed on dequeueing.
 *
 * In concept, every pointer has a condition that threads can wait on if
 * they are interested in knowing when that pointer moves. However, no
 * condition is needed for "claimed" because the transition to "completed"
 * occurs as work is done. The "enqueued" condition is at the level of the
 * thread pool rather than an individual queue because worker threads wait
 * on it for incoming work while not bound to any particular queue.
 *
 * Worker threads hold no locks while performing actual work.
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
	void			*zq_context;
	int			zq_batch_budget;
	zstream_queue_t		zq_forward_to;
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
}

static void
zstream_queue_destroy(zstream_queue_t queue);

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
		.zq_context = params->qp_context,
		.zq_batch_budget = params->qp_batch_budget,
		.zq_slots = safe_calloc(params->qp_queue_length *
			(sizeof(queue_slot_t) + params->qp_item_size)),
	};
	void *items_base = &queue->zq_slots[params->qp_queue_length];
	for (int i = 0; i < params->qp_queue_length; i++) {
		queue->zq_slots[i].qs_item =
			items_base + i * params->qp_item_size;
	}

	pthread_mutex_init(&queue->zq_mutex, NULL);
	pthread_cond_init(&queue->zq_completed, NULL);
	pthread_cond_init(&queue->zq_dequeued, NULL);

	pthread_mutex_unlock(&pool.tp_mutex);
	return queue;
}

/*
 * This periodic sweep is necessary because worker threads don't claim items
 * that require no work. They're marked as completed on enqueue, but the
 * zq_complete pointer still needs to move to declare them officially done.
 * However, no-work items don't arrive in any particular order. Whenever we
 * complete a batch or claim a batch, we advance the completion pointer past
 * all zero-work items.
 *
 * The calling thread must hold the queue lock.
 */
static void
advance_completion_pointer(zstream_queue_t queue) {
	boolean_t any_completed = B_FALSE;
	while (queue->zq_complete < queue->zq_claim) {
		int slot = queue->zq_complete % queue->zq_num_slots;
		if (queue->zq_slots[slot].qs_completed) {
			queue->zq_complete++;
			any_completed = B_TRUE;
		} else {
			break;
		}
	}
	if (any_completed) {
		pthread_cond_signal(&queue->zq_completed);
	}
}

static inline boolean_t
forward_items(zstream_queue_t sender, zstream_queue_t receiver, int num_items)
{
	boolean_t destroy_sender = B_FALSE;

	for (int j = 0; j < num_items; j++)
	{
		int from_slot = sender->zq_dequeue % sender->zq_num_slots;
		int to_slot = receiver->zq_enqueue % receiver->zq_num_slots;
		queue_slot_t *from = &sender->zq_slots[from_slot];
		queue_slot_t *to = &receiver->zq_slots[to_slot];

		queue_item *temp = to->qs_item;
		to->qs_item = from->qs_item;
		from->qs_item = temp;
		to->qs_end_of_stream = from->qs_end_of_stream;
		to->qs_cost = receiver->zq_cost(to->qs_item, receiver->zq_context);
		to->qs_completed = to->qs_cost == 0;
		destroy_sender = destroy_sender || from->qs_end_of_stream;
		sender->zq_dequeue++;
		receiver->zq_enqueue++;
	}
	return destroy_sender;
}

/*
 * Must be called with the pool mutext locked.
 */
static void
forward_completed_items(void)
{
	boolean_t destroy[pool.tp_num_queues] = {};
	boolean_t any_transferred = B_FALSE;
	boolean_t any_to_destroy = B_FALSE;

	for (int i = pool.tp_num_queues-1; i >= 0; i--) {
		zstream_queue_t sender = pool.tp_queues[i];
		zstream_queue_t receiver = sender->zq_forward_to;
		if (receiver) {
			int available = sender->zq_complete - sender->zq_dequeue;
			int recv_slots = receiver->zq_num_slots -
				(receiver->zq_enqueue - receiver->zq_dequeue);
			int transfers = MIN(available, recv_slots);
			if (transfers) {
				destroy[i] = forward_items(sender, receiver, transfers);
				any_to_destroy = any_to_destroy || destroy[i];
				any_transferred = B_TRUE;
				pthread_cond_signal(&sender->zq_dequeued);
			}
		}
	}
	if (any_to_destroy) {
		for (int i = pool.tp_num_queues-1; i >= 0; i--) {
			if (destroy[i]) {
				zstream_queue_destroy(pool.tp_queues[i]);
			}
		}
	}
	if (any_transferred) {
		pthread_cond_signal(&pool.tp_enqueued);
	}
}

/*
 * Identify the queue most in need of a worker thread and claim up to
 * MAX_BATCH work items, trying to accumulate at least queue->batch_budget
 * worth of work data (== "cost"). All items in a batch will be drawn from
 * the same queue.
 *
 * Does not block waiting to reach batch_budget; returns whatever is
 * available or awaits the any_queue_enqueued condition if nothing is
 * available.
 */
static int
claim_batch(zstream_queue_t queue, queue_slot_t **batch)
{
	uint64_t cost_claimed = 0;
	uint64_t count = 0;

	pthread_mutex_lock(&queue->zq_mutex);

	while (queue->zq_claim < queue->zq_enqueue &&
		count < MAX_BATCH &&
		((!queue->zq_batch_budget && !count) ||
			cost_claimed < queue->zq_batch_budget))
	{
		uint64_t slot_num = queue->zq_claim % queue->zq_num_slots;
		queue_slot_t *slot = &queue->zq_slots[slot_num];
		if (!slot->qs_completed) {
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
 * Score a queue according to its need for workers. Higher is better.
 *
 * Two measures are used for scoring. The "open score" is 1/M where M is the
 * number of slots available to receive new items. The "dequeue score" is
 * 1/N where N is the number of completed items available to dequeue. These
 * two are added together with the dequeue score scaled by
 * DEQUEUE_SCORE_WEIGHT.
 *
 * The total score is scaled by a factor that reflects how much work is
 * actually available to be claimed; no point sending threads to queues
 * without work.
 */
static inline double
score_queue(zstream_queue_t queue)
{
	uint64_t claimable = queue->zq_enqueue - queue->zq_claim;
	uint64_t dequeueable = queue->zq_complete - queue->zq_dequeue;
	uint64_t in_queue = queue->zq_enqueue - queue->zq_dequeue;
	uint64_t open_slots = queue->zq_num_slots - in_queue;

	double open_score = (open_slots > 0) ? (1.0 / open_slots) : 2.0;
	double dq_score = (dequeueable > 0) ? (1.0 / dequeueable) : 2.0;
	double claim_factor = MIN(claimable, PLENTY_OF_WORK) /
		(double)PLENTY_OF_WORK;
	double need = open_score + dq_score * DEQUEUE_SCORE_WEIGHT;

	return need * claim_factor;
}

static inline int
select_stochastic(double weights[], int num_values)
{
	uint32_t numerator;
	uint32_t denominator = 0xFFFFFFFF;
	double total = 0.0;

	for (int i = 0; i < num_values; i++) {
		total += weights[i];
	}
	random_get_bytes((uint8_t *)&numerator, sizeof(uint32_t));
	double select_val = total * numerator / denominator;
	for (int i = 0; i < num_values; i++) {
		if (select_val <= weights[i]) {
			return i;
		} else {
			select_val -= weights[i];
		}
	}
	abort();
}

/*
 * Threads are assigned to a queue on each loop so they can be shifted
 * dynamically to follow available work.
 *
 * Queues are scored without the queue mutex being held. Ergo, the numbers
 * may be skewed or out of date. However, the pool mutex prevents new work
 * from being enqueued while scoring is going on.
 */
static zstream_queue_t
assign_thread_to_queue(void)
{
	pthread_mutex_lock(&pool.tp_mutex);

	while (B_TRUE) {
		int num_queues = pool.tp_num_queues;
		double weights[MAX_QUEUES];
		int queues_with_work = 0;

		forward_completed_items();
		for (int i = 0; i < num_queues; i++) {
			weights[i] = score_queue(pool.tp_queues[i]);
			if (weights[i] > NO_WORK) {
				queues_with_work++;
			}
		}
		if (!num_queues || !queues_with_work) {
			await_condition(&pool.tp_enqueued, &pool.tp_mutex);
		} else {
			int q = select_stochastic(weights, num_queues);
			zstream_queue_t queue = pool.tp_queues[q];
			if (queues_with_work > 1) {
				pthread_cond_signal(&pool.tp_enqueued);
			}
			pthread_mutex_unlock(&pool.tp_mutex);
			return queue;
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
			queue->zq_process(batch[i]->qs_item, queue->zq_context);
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
zstream_enqueue_impl(zstream_queue_t queue, queue_item *item, boolean_t last)
{
	pthread_mutex_lock(&queue->zq_mutex);
	if (queue->zq_finalized) {
		pthread_mutex_unlock(&queue->zq_mutex);
		return;
	}
	while (queue->zq_enqueue - queue->zq_dequeue >= queue->zq_num_slots) {
		await_condition(&queue->zq_dequeued, &queue->zq_mutex);
	}
	int slot_num = queue->zq_enqueue % queue->zq_num_slots;
	queue_slot_t *slot = &queue->zq_slots[slot_num];
	slot->qs_cost = last ? 0 : queue->zq_cost(item, queue->zq_context);
	slot->qs_completed = slot->qs_cost == 0;
	slot->qs_end_of_stream = last;
	if (!last && item) {
		memcpy(slot->qs_item, item, queue->zq_item_size);
	}
	queue->zq_finalized = queue->zq_finalized || last;
	queue->zq_enqueue++;
	pthread_mutex_unlock(&queue->zq_mutex);

	/*
	 * Enqueueing doesn't require participation from the thread pool.
	 * However, we lock the thread pool before signaling the "enqueued"
	 * condition to play nicely with assign_thread_to_queue().
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
	zstream_enqueue_impl(queue, NULL, B_TRUE);
}

/*
 * Connect two queues so that items completed by the first are forwarded
 * automatically to the second. This avoids the need to have a separate
 * transfer thread and saves two copies per item.
 */
void
zstream_queue_forward(zstream_queue_t from, zstream_queue_t to) {
	VERIFY0(from->zq_forward_to);
	pthread_mutex_lock(&pool.tp_mutex);
	from->zq_forward_to = to;
	pthread_cond_signal(&pool.tp_enqueued);
	pthread_mutex_unlock(&pool.tp_mutex);
}

/*
 * Must be called with the pool mutex held.
 */
static void
zstream_queue_destroy(zstream_queue_t queue)
{
	pthread_mutex_destroy(&queue->zq_mutex);
	pthread_cond_destroy(&queue->zq_completed);
	pthread_cond_destroy(&queue->zq_dequeued);

	free(queue->zq_slots);
	queue->zq_slots = NULL;
	free(queue);

	if (pool.tp_num_queues == 1) {
		thread_pool_spindown();
	} else {
		boolean_t moving = B_FALSE;
		for (int i = 0; i < pool.tp_num_queues - 1; i++) {
			moving = moving || pool.tp_queues[i] == queue;
			if (moving) {
				pool.tp_queues[i] = pool.tp_queues[i+1];
			}
		}
	}
	pool.tp_num_queues--;
}

boolean_t
zstream_dequeue(zstream_queue_t queue, queue_item *item)
{
	pthread_mutex_lock(&queue->zq_mutex);
	while (queue->zq_dequeue >= queue->zq_complete) {
		await_condition(&queue->zq_completed, &queue->zq_mutex);
	}
	int slot_num = queue->zq_dequeue % queue->zq_num_slots;
	queue_slot_t *slot = &queue->zq_slots[slot_num];
	queue->zq_dequeue++;
	if (slot->qs_end_of_stream) {
		pthread_mutex_unlock(&queue->zq_mutex);
		pthread_mutex_lock(&pool.tp_mutex);
		zstream_queue_destroy(queue);
		pthread_mutex_unlock(&pool.tp_mutex);
		return B_FALSE;
	} else {
		memcpy(item, slot->qs_item, queue->zq_item_size);
		pthread_mutex_unlock(&queue->zq_mutex);
		pthread_cond_signal(&queue->zq_dequeued);
		return B_TRUE;
	}
}

