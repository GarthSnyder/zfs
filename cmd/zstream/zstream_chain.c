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

#include <assert.h>		/* VERIFY3S, VERIFY3U			*/
#include <libspl.h>		/* libspl_fini, libspl_init		*/
#include <pthread.h>		/* pthread_create, pthread_join		*/
#include <stdio.h>		/* fprintf, stderr, snprintf		*/
#include <stdlib.h>		/* exit					*/
#include <sys/abd.h>		/* abd_fini, abd_init			*/
#include <sys/param.h>		/* MAX, MIN				*/
#include <sys/zio.h>		/* zio_fini, zio_init			*/
#include <sys/zstd/zstd.h>	/* zstd_fini, zstd_init			*/
#include <zfs_fletcher.h>	/* fletcher_4_fini, fletcher_4_init	*/

#include "zstream_chain.h"	/* chain_attrs_t, record_stats_t...	*/
#include "zstream_queue.h"	/* zstream_queue_t, zstream_dequeue	*/

/*
 * Execute a chain of processing steps, some parallel and some serial.
 *
 * For simplicity, we normalize the chain item size to that of the largest
 * output of any step. Packets with data beyond the base drr_record_t should
 * add their additional data to the end of the packet, and this area may be
 * reused for different purposes as items travel down the chain.
 *
 * Execution is straightforward. First, zstream_queues are created for every
 * step that is to be performed in parallel.
 *
 * Second, service threads are spawned for sections of the chain. One thread
 * is assigned to every contiguous sequence of serial steps, plus the
 * parallel steps on either side, if any. Adjacent parallel steps also
 * receive a worker; this is just a special case of the same general
 * pattern, with the serial portion consisting of zero steps.
 *
 * Parallel steps are double-covered, which is the intended behavior. If a
 * worker's domain begins with a parallel step, it dequeues items from the
 * associated queue. If it ends with a parallel step, it submits items to
 * that queue.
 */

#define MAX_CHAIN_LENGTH 32

typedef struct chain_info {
	chain_step_t	*ci_chain;
	int		ci_num_steps;
	zstream_queue_t	**ci_queues;	/* Sparse */
	size_t		ci_item_size;
	chain_attrs_t	*ci_attrs;
} chain_info_t;

typedef struct {
	chain_info_t	*wc_chain_info;
	int		wc_first_step;	/* Responsibility region */
	int		wc_last_step;
} worker_context_t;

typedef void *pthread_worker(void *);

static void *
zstream_chain_worker(worker_context_t *context);

static void
chain_exec_serialized(chain_info_t *chain);

boolean_t serialize_chains = B_FALSE;

chain_step_t
serial_null_step(void)
{
	return ((chain_step_t) { .cs_type = CS_SERIAL });
}

chain_step_t
chain_terminator(void)
{
	return ((chain_step_t) { .cs_type = CS_TERMINATE });
}

static void
libraries_init(void)
{
	abd_init();
	zio_init();
	zstd_init();
	libspl_init();
	fletcher_4_init();
}

static void
libraries_fini(void)
{
	fletcher_4_fini();
	libspl_fini();
	zio_fini();
	zstd_fini();
	abd_fini();
}

void
zstream_chain_exec(zstream_chain_t chain, chain_attrs_t *attrs)
{
	int num_steps = 0;
	size_t max_size = 0;
	chain_attrs_t backup_attrs = {0};

	if (!attrs) {
		attrs = &backup_attrs;
	}

	while (chain[num_steps].cs_type != CS_TERMINATE) {
		if (num_steps > MAX_CHAIN_LENGTH) {
			fprintf(stderr, "Error: unterminated zstream_chain\n");
			exit(1);
		}
		max_size = MAX(max_size, chain[num_steps].cs_out_size);
		num_steps++;
	}

	zstream_queue_t	*queues[num_steps] = {};
	worker_context_t contexts[num_steps] = {};
	pthread_t worker_threads[num_steps] = {};
	int num_workers = 0;

	VERIFY3U(num_steps, >, 0);
	if (chain[num_steps-1].cs_type == CS_PARALLEL ||
	    chain[0].cs_type == CS_PARALLEL)
	{
		fprintf(stderr, "A zstream_chain cannot start or end "
		    "with a parallel step.\n");
		exit(1);
	}

	/*
	 * Check for consistency of input and output packet sizes in
	 * adjacent steps. A declared packet size of zero waives this check.
	 */
	for (int i = 0; i < num_steps; i++) {
		if (i > 0 &&
		    chain[i].cs_in_size != 0 &&
		    chain[i-1].cs_out_size != 0 &&
		    chain[i].cs_in_size != chain[i-1].cs_out_size)
		{
			fprintf(stderr, "Warning: chain steps %d and %d have "
			    "mismatched packet sizes\n", i - 1, i);
		}
	}

	libraries_init();
	struct chain_info chain_info = {
		.ci_chain	= chain,
		.ci_num_steps	= num_steps,
		.ci_queues	= queues,
		.ci_item_size	= max_size,
		.ci_attrs	= attrs
	};

	if (serialize_chains) {
		chain_exec_serialized(&chain_info);
		libraries_fini();
		return;
	}

	/* Create parallel queues */
	for (int i = 0; i < num_steps; i++) {
		if (chain[i].cs_type == CS_PARALLEL) {
			chain_step_t *ci = &chain[i];
			zq_params_t queue_params = {
				.qp_process	 = ci->cs_parallel.process,
				.qp_cost	 = ci->cs_parallel.cost,
				.qp_item_size	 = max_size,
				.qp_batch_budget = ci->cs_parallel.batch_budget,
				.qp_queue_length = ci->cs_parallel.queue_length,
				.qp_context	 = ci->cs_context
			};
			queues[i] = zstream_queue_create(&queue_params);
		}
	}

	/* Create worker context bundles and assign step ranges */
	int last = 0;
	for (int first = 0; first < num_steps - 1; first = last) {
		last = first + 1;
		while (last < num_steps && chain[last].cs_type != CS_PARALLEL) {
			last++;
		}
		last = MIN(last, num_steps - 1);
		contexts[num_workers].wc_chain_info = &chain_info;
		contexts[num_workers].wc_first_step = first;
		contexts[num_workers].wc_last_step = last;
		num_workers++;
	}

	/* Create threads */
	for (int i = 0; i < num_workers; i++) {
		char buff[32];
		int ret = pthread_create(&worker_threads[i], NULL,
			(pthread_worker *)zstream_chain_worker,
			&contexts[i]);
		VERIFY3S(ret, ==, 0);
		snprintf(buff, sizeof (buff), "chain-%d", i);
		pthread_setname_np(worker_threads[i], buff);
	}

	/* Await completion */
	for (int i = 0; i < num_workers; i++) {
		int ret = pthread_join(worker_threads[i], NULL);
		VERIFY3S(ret, ==, 0);
	}
	libraries_fini();
}

static void *
zstream_chain_worker(worker_context_t *ctxt)
{
	chain_info_t *ci = ctxt->wc_chain_info;
	uint8_t buffer[ci->ci_item_size];
	boolean_t done = B_FALSE;
	int i;

	while (!done) {
		for (i = ctxt->wc_first_step; i <= ctxt->wc_last_step; i++) {
			chain_step_t *step = &ci->ci_chain[i];
			zstream_queue_t *queue = ci->ci_queues[i];
			if (step->cs_type == CS_SERIAL) {
				done = !step->cs_serial.process(
				    done ? NULL : buffer,
				    step->cs_context,
				    ci->ci_attrs) || done;
			} else if (i == ctxt->wc_first_step) {
				done = done || !zstream_dequeue(queue, buffer);
			} else if (done) {
				zstream_queue_fini(queue);
			} else {
				zstream_enqueue(queue, buffer);
			}
		}
	}
	return (NULL);
}

/*
 * Execute a chain linearly, without queues and without multithreading. For
 * debugging. This option is triggered by setting serialize_chains to
 * B_TRUE.
 */
static void
chain_exec_serialized(chain_info_t *ci)
{
	uint8_t buffer[ci->ci_item_size];
	boolean_t done = B_FALSE;

	while (!done) {
	    for (int i = 0; i < ci->ci_num_steps; i++) {
		if (ci->ci_chain[i].cs_type == CS_SERIAL) {
			uint8_t *arg = done ? NULL : buffer;
			done = !ci->ci_chain[i].cs_serial.process(arg,
			    ci->ci_chain[i].cs_context, ci->ci_attrs) || done;
		} else if (!done) {
			size_t cost = ci->ci_chain[i].cs_parallel.cost(buffer,
			    ci->ci_chain[i].cs_context);
			if (cost > 0) {
				ci->ci_chain[i].cs_parallel.process(buffer,
				    ci->ci_chain[i].cs_context);
			}
		}
	    }
	}
}

