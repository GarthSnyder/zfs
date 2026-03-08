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
#include "zstream_util.h"

#define MIN_THREADS 	6
#define MAX_QUEUES 	16	/* Greatest # of queues simultaneously active */

#define PLENTY_OF_WORK	       6	/* "Many" items to claim */
#define NO_WORK		       0.0001	/* No-work score threshold */
#define DEQUEUE_SCORE_WEIGHT   0.3	/* Dequeue score relative weight */

#define Q_MOD(queue, index)    (queue->index % queue->zq_params.qp_queue_length)
#define Q_SLOT(queue, index)   (queue->zq_slots[Q_MOD(queue, index)])

/*
 * A zstream_queue is a ring buffer with four indices: enqueue, claim,
 * complete, and dequeue, in that order. No index can move beyond its
 * preceding index. Every interval between indices contains work items in a
 * particular state: enqueued, claimed for work, completed. Since items
 * never leave the ring buffer, FIFO order is guaranteed on dequeueing.
 *
 * In concept, every index has a corresponding condition that threads can
 * wait on if they are interested in knowing when that index moves. However,
 * no condition is needed for "claimed" because the transition to
 * "completed" occurs as work is done. That is, the same thread moves both
 * the "claimed" index and the "completed" index at different phases of the
 * same operation.
 *
 * The "enqueued" condition is shared among all queues and lives at the
 * level of the thread pool. That's because threads are not associated with
 * any particular queue while waiting for work.
 *
 * Worker threads hold no locks while performing actual work.
 */

typedef struct {
	queue_item	*qs_item;
	size_t		qs_cost;
	boolean_t	qs_completed;
	boolean_t	qs_end_of_stream;
} queue_slot_t;

typedef struct {
	int		max_depth;
	int		min_depth;
} zq_stats_t;

typedef struct {
	int		enqueue;
	int		claim;
	int		complete;
	int		dequeue;
} zq_indices_t;

typedef struct {
	pthread_cond_t	completed;
	pthread_cond_t	dequeued;
} zq_conditions_t;

struct zstream_queue {
	queue_slot_t		*zq_slots;
	pthread_mutex_t		zq_mutex;
	zq_indices_t		zq_ix;
	zq_conditions_t		zq_cond;
	zq_params_t		zq_params;
#ifdef MONITOR_QUEUES
	zq_stats_t		zq_stats;
#endif
	boolean_t		zq_disallow_enqueue;
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

static void *
queue_worker(void *);

#ifdef MONITOR_QUEUES
static void
start_monitor_thread(void);
#endif

static thread_pool_t	pool = {};
static pthread_once_t	once_control = PTHREAD_ONCE_INIT;

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
#ifdef CPU_COUNT
	cpu_set_t cpu_set;
	sched_getaffinity(0, sizeof (cpu_set_t), &cpu_set);
	pool.tp_num_threads = CPU_COUNT(&cpu_set);
#else
	pool.tp_num_threads = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	pool.tp_num_threads = MAX(pool.tp_num_threads, MIN_THREADS);
	pool.tp_threads = safe_malloc(sizeof (pthread_t) * pool.tp_num_threads);
	for (int i = 0; i < pool.tp_num_threads; i++) {
		pthread_create(&pool.tp_threads[i], NULL, queue_worker, NULL);
		char buff[32];
		sprintf(buff, "queue-%d", i);
		pthread_setname_np(pool.tp_threads[i], buff);
		pthread_detach(pool.tp_threads[i]);
	}
#ifdef MONITOR_QUEUES
	start_monitor_thread();
#endif
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

zstream_queue_t *
zstream_queue_create(zq_params_t *params)
{
	pthread_once(&once_control, thread_pool_init);
	pthread_mutex_lock(&pool.tp_mutex);

	if (!pool.tp_num_threads) {
		thread_pool_spinup();
	}
	zstream_queue_t *queue = safe_malloc(sizeof (struct zstream_queue));
	pool.tp_queues[pool.tp_num_queues++] = queue;

	*queue = (zstream_queue_t) {
		.zq_params = *params,
		.zq_slots = safe_calloc(params->qp_queue_length *
		    (sizeof (queue_slot_t) + params->qp_item_size))
	};
	/*
	 * Queue slots and item storage are allocated in one block. Connect
	 * each slot to its item.
	 */
	void *item = &queue->zq_slots[params->qp_queue_length];
	queue_slot_t *slot = &queue->zq_slots[0];
	for (int i = 0; i < params->qp_queue_length; i++) {
		slot->qs_item = item;
		item += queue->zq_params.qp_item_size;
		slot++;
	}

	pthread_mutex_init(&queue->zq_mutex, NULL);
	pthread_cond_init(&queue->zq_cond.completed, NULL);
	pthread_cond_init(&queue->zq_cond.dequeued, NULL);

	pthread_mutex_unlock(&pool.tp_mutex);
	return (queue);
}

/*
 * This sweep is necessary because worker threads don't claim items that
 * require no work. They're marked as completed on enqueue, but the
 * "complete" index still needs to move to declare them officially done.
 * However, no-work items don't arrive in any particular order. Whenever we
 * complete a batch or claim a batch, we advance the completion index past
 * all completed items.
 *
 * The calling thread must hold the queue lock.
 */
static void
advance_completion_index(zstream_queue_t *queue) {
	boolean_t any_completed = B_FALSE;
	while (queue->zq_ix.complete < queue->zq_ix.claim &&
	    Q_SLOT(queue, zq_ix.complete).qs_completed)
	{
		queue->zq_ix.complete++;
		any_completed = B_TRUE;
	}
	if (any_completed) {
		pthread_cond_signal(&queue->zq_cond.completed);
	}
}

/*
 * Claim up to MAX_BATCH work items from the given queue, trying to
 * accumulate at least queue->qp_batch_budget worth of work data (== "cost").
 * All items in a batch will be drawn from the same queue.
 *
 * Does not block waiting to fill the budget; returns whatever is
 * available now.
 */
static int
claim_batch(zstream_queue_t *queue, queue_slot_t **batch)
{
	uint64_t cost_claimed = 0;
	uint64_t count = 0;
	boolean_t more_to_claim, more_slots, more_budget;
	boolean_t first_and_only, ok_to_claim;

	pthread_mutex_lock(&queue->zq_mutex);

	while (B_TRUE)
	{
		more_to_claim = queue->zq_ix.claim < queue->zq_ix.enqueue;
		more_slots = count < MAX_BATCH;
		more_budget = cost_claimed < queue->zq_params.qp_batch_budget;
		first_and_only = queue->zq_params.qp_batch_budget == 0 &&
		    count == 0;
		ok_to_claim = first_and_only || more_budget;

		if (!more_to_claim || !more_slots || !ok_to_claim) {
			break;
		}
		queue_slot_t *slot = &Q_SLOT(queue, zq_ix.claim);
		if (!slot->qs_completed) {
			cost_claimed += slot->qs_cost;
			batch[count++] = slot;
		}
		queue->zq_ix.claim++;
	}

	advance_completion_index(queue);
	pthread_mutex_unlock(&queue->zq_mutex);
	return (count);
}

/*
 * Score a queue according to its need for workers. Higher is better.
 *
 * Two measures are used for scoring. The "open score" is 1/M where M is the
 * number of slots available to receive new items. The "dequeue score" is
 * 1/N where N is the number of completed items available to dequeue. These
 * two measures are added together with the dequeue score scaled by
 * DEQUEUE_SCORE_WEIGHT.
 *
 * The total score is scaled by a factor that reflects how much work is
 * actually available to be claimed; no point sending threads to queues
 * without work.
 *
 * Queues are scored without the queue mutex being held. Ergo, the numbers
 * may be skewed or out of date. However, the pool mutex prevents new work
 * from being enqueued while scoring is going on, so this will not result in
 * newly-enqueued work being overlooked.
 *
 * The calling thread must hold the pool mutex.
 */
static inline double
score_queue(zstream_queue_t *queue)
{
	uint64_t claimable = queue->zq_ix.enqueue - queue->zq_ix.claim;
	uint64_t dequeueable = queue->zq_ix.complete - queue->zq_ix.dequeue;
	uint64_t in_queue = queue->zq_ix.enqueue - queue->zq_ix.dequeue;
	uint64_t open_slots = queue->zq_params.qp_queue_length - in_queue;

	double open_score = (open_slots > 0) ? (1.0 / open_slots) : 2.0;
	double dq_score = (dequeueable > 0) ? (1.0 / dequeueable) : 2.0;
	double claim_factor = MIN(claimable, PLENTY_OF_WORK) /
		(double)PLENTY_OF_WORK;
	double need = open_score + dq_score * DEQUEUE_SCORE_WEIGHT;
	return (need * claim_factor);
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
	random_get_pseudo_bytes((uint8_t *)&numerator, sizeof (uint32_t));
	double select_val = total * numerator / denominator;
	for (int i = 0; i < num_values; i++) {
		if (select_val <= weights[i]) { return (i); }
		select_val -= weights[i];
	}
	abort();
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

/*
 * Threads are assigned to a queue on each loop so they can be shifted
 * dynamically to follow available work.
 */
static int
assign_queue_and_get_work(zstream_queue_t **queue, queue_slot_t **batch)
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
		if (!queues_with_work) {
			await_condition(&pool.tp_enqueued, &pool.tp_mutex);
		} else {
			int q = select_stochastic(weights, num_queues);
			*queue = pool.tp_queues[q];
			pthread_mutex_unlock(&pool.tp_mutex);
			int count = claim_batch(*queue, batch);
			if ((*queue)->zq_ix.claim < (*queue)->zq_ix.enqueue ||
				queues_with_work > 1)
			{
				pthread_cond_signal(&pool.tp_enqueued);
			}
			return (count);
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

start:	count = assign_queue_and_get_work(&queue, batch);
	if (count) {
		zq_process_item_f *process = queue->zq_params.qp_process;
		void *context = queue->zq_params.qp_context;
		atomic_add_32(&items_claimed, count);
		/*
		 * Complete the whole batch without holding any locks. We
		 * can't mark items as completed without holding the queue
		 * lock because that creates a race condition with
		 * advance_completion_index().
		 */
		for (int i = 0; i < count; i++) {
			process(batch[i]->qs_item, context);
		}
		pthread_mutex_lock(&queue->zq_mutex);
		for (int i = 0; i < count; i++) {
			batch[i]->qs_completed = B_TRUE;
		}
		advance_completion_index(queue);
		atomic_sub_32(&items_claimed, count);
		pthread_mutex_unlock(&queue->zq_mutex);
	}
	goto start;
	return (NULL);
}

/*
 * Implements both _enqueue and _fini. item == NULL for fini.
 */
void
zstream_enqueue(zstream_queue_t *queue, queue_item *item)
{
	pthread_mutex_lock(&queue->zq_mutex);
	ASSERT3B(queue->zq_disallow_enqueue, ==, B_FALSE);

	while (queue->zq_ix.enqueue - queue->zq_ix.dequeue >=
	    queue->zq_params.qp_queue_length)
	{
		await_condition(&queue->zq_cond.dequeued, &queue->zq_mutex);
	}

	queue_slot_t *slot = &Q_SLOT(queue, zq_ix.enqueue);
	if (item) {
		slot->qs_cost =
		    queue->zq_params.qp_cost(item, queue->zq_params.qp_context);
		slot->qs_completed = slot->qs_cost == 0;
		memcpy(slot->qs_item, item, queue->zq_params.qp_item_size);
	} else {
		slot->qs_cost = 0;
		slot->qs_completed = B_TRUE;
		slot->qs_end_of_stream = B_TRUE;
		queue->zq_disallow_enqueue = B_TRUE;
	}
	queue->zq_ix.enqueue++;

#ifdef MONITOR_QUEUES
	/* Maintain queue usage data per monitor interval */
	int depth = queue->zq_ix.enqueue - queue->zq_ix.dequeue;
	queue->zq_stats.max_depth = MAX(queue->zq_stats.max_depth, depth);
	queue->zq_stats.min_depth = MIN(queue->zq__stats.min_depth, depth);
#endif

	pthread_mutex_unlock(&queue->zq_mutex);

	/*
	 * Enqueueing doesn't require participation from the thread pool.
	 * However, we lock the thread pool before signaling the "enqueued"
	 * condition to play nicely with assign_queue_and_get_work().
	 */
	pthread_mutex_lock(&pool.tp_mutex);
	pthread_cond_signal(&pool.tp_enqueued);
	pthread_mutex_unlock(&pool.tp_mutex);
}

void
zstream_queue_fini(zstream_queue_t *queue) {
	zstream_enqueue(queue, NULL);
}

/*
 * Must be called with the pool mutex held.
 */
static void
zstream_queue_destroy(zstream_queue_t *queue)
{
	pthread_mutex_destroy(&queue->zq_mutex);
	pthread_cond_destroy(&queue->zq_cond.completed);
	pthread_cond_destroy(&queue->zq_cond.dequeued);

	free(queue->zq_slots);
	queue->zq_slots = NULL;
	free(queue);

	if (pool.tp_num_queues == 1) {
		thread_pool_spindown();
	} else {
		/* Gaps are not allowed in the tp_queues array */
		zstream_queue_t **qscan = &pool.tp_queues[0];
		int i = pool.tp_num_queues - 1;
		while (*qscan != queue) { qscan++; i--; }
		memmove(qscan, qscan + 1, i * sizeof (*qscan));
	}
	pool.tp_num_queues--;
}

boolean_t
zstream_dequeue(zstream_queue_t *queue, queue_item *item)
{
	pthread_mutex_lock(&queue->zq_mutex);
	while (queue->zq_ix.dequeue >= queue->zq_ix.complete) {
		await_condition(&queue->zq_cond.completed, &queue->zq_mutex);
	}
	queue_slot_t *slot = &Q_SLOT(queue, zq_ix.dequeue);
	queue->zq_ix.dequeue++;
	if (slot->qs_end_of_stream) {
		pthread_mutex_unlock(&queue->zq_mutex);
		pthread_mutex_lock(&pool.tp_mutex);
		zstream_queue_destroy(queue);
		pthread_mutex_unlock(&pool.tp_mutex);
		return (B_FALSE);
	} else {
		memcpy(item, slot->qs_item, queue->zq_params.qp_item_size);
		pthread_mutex_unlock(&queue->zq_mutex);
		pthread_cond_signal(&queue->zq_cond.dequeued);
		return (B_TRUE);
	}
}

#ifdef MONITOR_QUEUES

#define JIFFIES_PER_SEC 100
#define SAMPLE_DURATION_US 1000000

/*
 * Monitor queue and CPU usage. This is all likely Linux-specific, but it's
 * needed only while tuning queue lengths and batch sizes.
 */
static void *
cpu_and_queue_monitor(void *dummy)
{
	(void) dummy;
	uint64_t period = SAMPLE_DURATION_US;
	struct timespec clock = {};
	uint64_t start_us, end_us, delta_jif;
	uint64_t cpu_jif_prior = 0;
	uint64_t delta_cpu_jif;
	long unsigned int utime, stime;
	char buff[1024];
	boolean_t interrupt = B_FALSE;
	FILE *fp = fopen("/proc/self/stat", "r");
	VERIFY3P(fp, !=, NULL);

	/* Wait a few seconds for things to settle into steady state */
	usleep(3 * 1000 * 1000);

start:	usleep(period);
	fp = fopen("/proc/self/stat", "r");
	VERIFY3P(fp, !=, NULL);
	VERIFY3P(fgets(buff, sizeof (buff), fp), !=, NULL);
	fclose(fp);
	char *p = strrchr(buff, ')');
	VERIFY3P(p, !=, NULL);
	p += 2;  /* skip ") " and fields 3-13 */
	for (int i = 0; i < 11; i++) {
		p = strchr(p, ' ');
		VERIFY3P(p, !=, NULL);
		p++;
	}
	VERIFY3U(sscanf(p, "%lu %lu", &utime, &stime), ==, 2);

	pthread_mutex_lock(&pool.tp_mutex);
	if (cpu_jif_prior) {
		delta_cpu_jif = utime + stime - cpu_jif_prior;
		clock_gettime(CLOCK_MONOTONIC, &clock);
		end_us = clock.tv_sec * 1000000 + clock.tv_nsec / 1000;
		delta_jif = (end_us - start_us) / (JIFFIES_PER_SEC * 100);
		double cpu_pct = (double)delta_cpu_jif /
		    (pool.tp_num_threads * delta_jif);
		fprintf(stderr, "CPU: %.2f%%  ", 100 * cpu_pct);
		/* Stop to investigate low CPU usage */
		if (interrupt && cpu_pct < 0.85 && cpu_pct > 0.1) {
			kill(getpid(), SIGSTOP);
		}
	}

	/* Report queue depths */
	for (int i = 0; i < pool.tp_num_queues; i++) {
		zstream_queue_t *q = pool.tp_queues[i];
		fprintf(stderr, "Queue %d: %d-%d  ", i, q->zq_min_depth,
			q->zq_max_depth);
		q->zq_min_depth = 999999999;
		q->zq_max_depth = 0;
	}

	pthread_mutex_unlock(&pool.tp_mutex);
	fprintf(stderr, "\n");
	cpu_jif_prior = utime + stime;
	start_us = end_us;
	goto start;
	return (NULL);
}

static void
start_monitor_thread(void) {
	pthread_t monitor;
	pthread_create(&monitor, NULL, cpu_and_queue_monitor, NULL);
	pthread_setname_np(monitor, "monitor-0");
	pthread_detach(monitor);
}

#endif  /* MONITOR_QUEUES */
