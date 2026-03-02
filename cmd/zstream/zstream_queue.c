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
	int			zq_max_depth;
	int			zq_min_depth;
	zstream_queue_t		*zq_forward_to;
	boolean_t		zq_is_forwarded_to;
	boolean_t		zq_finalized;
};

typedef struct {
	pthread_mutex_t		tp_mutex;
	pthread_cond_t		tp_enqueued;
	zstream_queue_t		*tp_queues[MAX_QUEUES];
	int			tp_num_queues;
	pthread_t		*tp_threads;
	int			tp_num_threads;
} thread_pool_t;

typedef void cleanup_f(void *);
typedef void *thread_f(void *);

static thread_pool_t pool = {};
static pthread_once_t once_control = PTHREAD_ONCE_INIT;

static void *queue_worker(void *);
static void *forwarding_worker(zstream_queue_t *from);

static void
start_monitor_thread(void);

static void
thread_pool_init(void) {
	pthread_mutex_init(&pool.tp_mutex, NULL);
	pthread_cond_init(&pool.tp_enqueued, NULL);
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
	char buff[32];
	static int worker_number = 0;
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
		sprintf(buff, "queue-%d", worker_number++);
		pthread_setname_np(pool.tp_threads[i], buff);
		pthread_detach(pool.tp_threads[i]);
	}
	start_monitor_thread();
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
	pthread_cleanup_push((cleanup_f *)auto_unlock_mutex, mutex);
	pthread_cond_wait(cond, mutex);
	pthread_cleanup_pop(0);
}

zstream_queue_t *
zstream_queue_create(zq_params_t *params)
{
	pthread_once(&once_control, thread_pool_init);
	pthread_mutex_lock(&pool.tp_mutex);
	if (!pool.tp_num_threads) {
		thread_pool_spinup();
	}
	zstream_queue_t *queue = safe_malloc(sizeof(struct zstream_queue));
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
advance_completion_pointer(zstream_queue_t *queue) {
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
claim_batch(zstream_queue_t *queue, queue_slot_t **batch)
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
score_queue(zstream_queue_t *queue)
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
	random_get_pseudo_bytes((uint8_t *)&numerator, sizeof(uint32_t));
	double select_val = total * numerator / denominator;
	for (int i = 0; i < num_values; i++) {
		if (select_val <= weights[i]) { return i; }
		select_val -= weights[i];
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
static int
assign_queue_and_claim_batch(zstream_queue_t **queue, queue_slot_t **batch)
{
	pthread_mutex_lock(&pool.tp_mutex);

	while (B_TRUE) {
		int num_queues = pool.tp_num_queues;
		double weights[MAX_QUEUES];
		int queues_with_work = 0;

		for (int i = 0; i < num_queues; i++) {
			weights[i] = score_queue(pool.tp_queues[i]);
			if (weights[i] > NO_WORK) queues_with_work++;
		}
		if (!num_queues || !queues_with_work) {
			await_condition(&pool.tp_enqueued, &pool.tp_mutex);
		} else {
			int q = select_stochastic(weights, num_queues);
			*queue = pool.tp_queues[q];
			pthread_mutex_unlock(&pool.tp_mutex);
			int count = claim_batch(*queue, batch);
			if ((*queue)->zq_claim < (*queue)->zq_enqueue ||
				queues_with_work > 1)
			{
				pthread_cond_signal(&pool.tp_enqueued);
			}
			return count;
		}
	}
}

volatile uint32_t items_claimed = {};

static void *
queue_worker(void *dummy)
{
	(void) dummy;
	zstream_queue_t *queue;
	queue_slot_t *batch[MAX_BATCH];
	int count;

start:	if ((count = assign_queue_and_claim_batch(&queue, batch))) {
		atomic_add_32(&items_claimed, count);
		/* Complete the whole batch before returning any items */
		for (int i = 0; i < count; i++) {
			queue->zq_process(batch[i]->qs_item, queue->zq_context);
		}
		pthread_mutex_lock(&queue->zq_mutex);
		for (int i = 0; i < count; i++) {
			batch[i]->qs_completed = B_TRUE;
		}
		advance_completion_pointer(queue);
		atomic_sub_32(&items_claimed, count);
		pthread_mutex_unlock(&queue->zq_mutex);
	}
	goto start;
	return NULL;
}

static void
zstream_enqueue_impl(zstream_queue_t *queue, queue_item *item, boolean_t last)
{
	pthread_mutex_lock(&queue->zq_mutex);
	if (queue->zq_finalized) {
		pthread_mutex_unlock(&queue->zq_mutex);
		return;
	}
	VERIFY3U(queue->zq_is_forwarded_to, ==, B_FALSE);
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

	int depth = queue->zq_enqueue - queue->zq_dequeue;
	queue->zq_max_depth = MAX(queue->zq_max_depth, depth);
	queue->zq_min_depth = MIN(queue->zq_min_depth, depth);

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
zstream_enqueue(zstream_queue_t *queue, queue_item *item) {
	zstream_enqueue_impl(queue, item, B_FALSE);
}

void
zstream_queue_fini(zstream_queue_t *queue) {
	zstream_enqueue_impl(queue, NULL, B_TRUE);
}

/*
 * Connect two queues so that items completed by the first are forwarded
 * automatically to the second. The advantage over just looping through
 * "dequeue from A, enqueue on B" is that it avoids two copies. It's also
 * potentially cheaper in terms of locking and synchronization activity
 * since multiple items can be forwarded at once.
 */
void
zstream_queue_forward(zstream_queue_t *from, zstream_queue_t *to)
{
	pthread_t 	forward_thread;
	static int	forwarder_number = 0;
	char	  	buff[32];

	if (from->zq_item_size != to->zq_item_size) {
		fprintf(stderr, "Cannot forward between two queues "
			"with different item sizes.\n");
		exit(1);
	}
	if (to->zq_is_forwarded_to) {
		fprintf(stderr, "It is not currently possible to forward "
			"items from multiple queues to the same destination.\n");
		exit(1);
	}
	pthread_mutex_lock(&from->zq_mutex);
	VERIFY0(from->zq_forward_to);
	from->zq_forward_to = to;
	pthread_mutex_unlock(&from->zq_mutex);
	pthread_create(&forward_thread, NULL, (thread_f *)forwarding_worker, from);
	sprintf(buff, "forwarder-%d", forwarder_number++);
	pthread_setname_np(forward_thread, buff);
	pthread_detach(forward_thread);
}

/*
 * Must be called with the pool mutex held.
 */
static void
zstream_queue_destroy(zstream_queue_t *queue)
{
	pthread_mutex_destroy(&queue->zq_mutex);
	pthread_cond_destroy(&queue->zq_completed);
	pthread_cond_destroy(&queue->zq_dequeued);

	// free(queue->zq_slots);
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
zstream_dequeue(zstream_queue_t *queue, queue_item *item)
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

/*
 * Forward as many items as possible from one queue to another. Returns
 * B_TRUE if the source queue reaches end-of-stream.
 *
 * Must be called with both queues locked.
 */
static boolean_t
forward_items(zstream_queue_t *from, zstream_queue_t *to)
{
	int from_slots = from->zq_complete - from->zq_dequeue;
	int to_occupied = to->zq_enqueue - to->zq_dequeue;
	int to_slots = to->zq_num_slots - to_occupied;
	int batch = MIN(from_slots, to_slots);

	for (int i = 0; i < batch; i++) {
		int from_ix = from->zq_dequeue++ % from->zq_num_slots;
		int to_ix = to->zq_enqueue++ % to->zq_num_slots;
		queue_slot_t *from_slot = &from->zq_slots[from_ix];
		queue_slot_t *to_slot = &to->zq_slots[to_ix];
		void *temp = to_slot->qs_item;

		to_slot->qs_item = from_slot->qs_item;
		from_slot->qs_item = temp;
		to_slot->qs_end_of_stream = from_slot->qs_end_of_stream;

		if (from_slot->qs_end_of_stream) {
			to->zq_finalized = B_TRUE;
			to_slot->qs_cost = 0;
			to_slot->qs_completed = B_TRUE;
			return B_TRUE;
		}

		to_slot->qs_end_of_stream = B_FALSE;
		to_slot->qs_cost = to->zq_cost(to_slot->qs_item, to->zq_context);
		to_slot->qs_completed = to_slot->qs_cost == 0;
	}
	return B_FALSE;
}

/*
 * A forwarder thread is the only dequeuer on the "from" queue, and it is
 * also the only enqueuer on the "to" queue. So we can independently check
 * the number of items available to be forwarded and the number of slots
 * available to receive them. From this, we calculate a floor on the number
 * of transferrable items. Only when we're sure we can do a transfer without
 * blocking on either side do we secure both locks.
 *
 * Lock ordering: the "to" queue lock is always secured first, followed by
 * the "from" queue. The thread pool mutex is acquired last of all.
 *
 * This is the only case in this queue implementation in which multiple
 * locks are held simultaneously by a single thread.
 */
static void *
forwarding_worker(zstream_queue_t *from)
{
	zstream_queue_t *to = from->zq_forward_to;

start:	pthread_mutex_lock(&from->zq_mutex);
	while (from->zq_complete - from->zq_dequeue == 0) {
		pthread_cond_wait(&from->zq_completed, &from->zq_mutex);
	}
	pthread_mutex_unlock(&from->zq_mutex);
	pthread_mutex_lock(&to->zq_mutex);
	while (to->zq_num_slots - (to->zq_enqueue - to->zq_dequeue) == 0) {
		pthread_cond_wait(&to->zq_dequeued, &to->zq_mutex);
	}
	pthread_mutex_lock(&from->zq_mutex);

	boolean_t from_at_eos = forward_items(from, to);

	int depth = to->zq_enqueue - to->zq_dequeue;
	to->zq_max_depth = MAX(to->zq_max_depth, depth);
	to->zq_min_depth = MIN(to->zq_min_depth, depth);

	pthread_mutex_unlock(&to->zq_mutex);
	pthread_mutex_lock(&pool.tp_mutex);
	pthread_cond_signal(&pool.tp_enqueued);

	if (from_at_eos) {
		pthread_mutex_unlock(&from->zq_mutex);
		zstream_queue_destroy(from);
		pthread_mutex_unlock(&pool.tp_mutex);
		return NULL;
	}

	pthread_mutex_unlock(&pool.tp_mutex);
	pthread_cond_signal(&from->zq_dequeued);
	pthread_mutex_unlock(&from->zq_mutex);

	goto start;
}


#define JIFFIES_PER_SEC 100
#define SAMPLE_DURATION_US 1000000

static void *
cpu_utilization_monitor(void *dummy)
{
	(void) dummy;
	uint64_t period = SAMPLE_DURATION_US;
	struct timespec clock = {};
	uint64_t start_us, end_us, delta_jif;
	long unsigned int time_base = 0;
	long unsigned int utime, stime;
	long unsigned int delta_stat;
	char buff[1024];
	boolean_t interrupt = B_FALSE;

	usleep(5 * 1000 * 1000);
	while (B_TRUE) {
		usleep(period);
		FILE *fp = fopen("/proc/self/stat", "r");
		VERIFY3P(fp, !=, NULL);
		VERIFY3P(fgets(buff, sizeof(buff), fp), !=, NULL);
		fclose(fp);
		char *p = strrchr(buff, ')');
		if (p == NULL) abort();
		p += 2;  /* skip ") " */
		/* skip fields 3-13 (11 fields) */
		for (int i = 0; i < 11; i++) {
		    p = strchr(p, ' ');
		    if (p == NULL)
		        abort();
		    p++;
		}
		if (sscanf(p, "%lu %lu", &utime, &stime) != 2)
		    abort();
		if (time_base) {
			delta_stat = utime + stime - time_base;
			clock_gettime(CLOCK_MONOTONIC, &clock);
			end_us = clock.tv_sec * 1000000 + clock.tv_nsec / 1000;
			delta_jif = (end_us - start_us) / 10000;
			double cpu_pct = (double)delta_stat / delta_jif;
			fprintf(stderr, "CPU utilization: %.0f%%  ", 100*cpu_pct);
			if (interrupt && cpu_pct < 12.0 && cpu_pct > 1.0) {
				kill(getpid(), SIGSTOP);
			}
		}
		pthread_mutex_lock(&pool.tp_mutex);
		for (int i = 0; i < pool.tp_num_queues; i++) {
			zstream_queue_t *q = pool.tp_queues[i];
			fprintf(stderr, "Queue %d: %d-%d  ", i, q->zq_min_depth,
				q->zq_max_depth);
			q->zq_min_depth = 999999999;
			q->zq_max_depth = 0;
		}
		pthread_mutex_unlock(&pool.tp_mutex);
		fprintf(stderr, "\n");
		time_base = utime + stime;
		start_us = end_us;
	}
}

static void
start_monitor_thread(void) {
	pthread_t monitor;
	pthread_create(&monitor, NULL, cpu_utilization_monitor, NULL);
	pthread_setname_np(monitor, "monitor-0");
	pthread_detach(monitor);
}
