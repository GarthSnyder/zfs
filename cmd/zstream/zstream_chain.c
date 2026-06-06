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
#include <err.h>
#include <libspl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/abd.h>
#include <sys/param.h>
#include <sys/stdtypes.h>
#include <sys/zio.h>
#include <sys/zstd/zstd.h>
#include <zfs_fletcher.h>

#include "zstream_chain.h"

#define	MAX_CHAIN_LENGTH 32

typedef struct chain_info {
	chain_step_t	*ci_chain;
	int		ci_num_steps;
	zstream_queue_t	**ci_queues;	/* Sparse */
	size_t		ci_item_size;
} chain_info_t;

typedef struct {
	chain_info_t	*wc_chain_info;
	int		wc_first_step;	/* Responsibility region */
	int		wc_last_step;
} worker_context_t;

typedef void *chain_worker(void *);

chain_attrs_t *chain_attrs;
boolean_t serialize_chains = B_FALSE;

static void *
zstream_chain_worker(worker_context_t *context);

static void
chain_exec_serialized(chain_info_t *chain);

static disposition_t
chain_null_step(void *item, void *context)
{
	(void) item;
	(void) context;
	return (D_OK);
}

chain_step_t
serial_null_step(void)
{
	chain_step_t step = {
		.cs_type = CS_SERIAL,
		.cs_serial = {
		    .process = (zc_serial_process_f *)chain_null_step
		}
	};
	return (step);
}

chain_step_t
chain_terminator(void)
{
	chain_step_t step = { .cs_type = CS_TERMINATE };
	return (step);
}

static void
libraries_init(void)
{
	zfs_refcount_init();
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
	zfs_refcount_fini();
}

/*
 * Validate chain and calculate number of steps and max packet size.
 */
static void
validate_chain(struct chain_info *info)
{
	int num_steps = 0;
	size_t packet_size = 0;
	zstream_chain_t chain = info->ci_chain;

	while (chain[num_steps].cs_type != CS_TERMINATE) {
		if (num_steps > MAX_CHAIN_LENGTH) {
			errx(1, "unterminated zstream_chain");
		}
		packet_size = MAX(packet_size, chain[num_steps].cs_out_size);
		num_steps++;
	}
	VERIFY3U(num_steps, >, 0);

	if (chain[num_steps-1].cs_type == CS_PARALLEL ||
	    chain[0].cs_type == CS_PARALLEL)
	{
		errx(1, "a zstream_chain cannot start or end "
		    "with a parallel step");
	}

	/*
	 * Check for consistency of input and output packet sizes in
	 * adjacent steps. A declared packet size of zero waives this check.
	 */
	for (int i = 0; i < num_steps; i++) {
		boolean_t mismatch = i > 0 &&
		    chain[i].cs_in_size != 0 &&
		    chain[i-1].cs_out_size != 0 &&
		    chain[i].cs_in_size != chain[i-1].cs_out_size;
		if (mismatch) {
			warnx("note - chain steps %d and %d have "
			    "mismatched packet sizes", i - 1, i);
		}
	}

	info->ci_item_size = packet_size;
	info->ci_num_steps = num_steps;
}

/*
 * Execute a chain of processing steps, some parallel and some serial.
 *
 * For simplicity, we normalize the chain item size to that of the largest
 * output of any step. Packets with data beyond the base drr_record_t should
 * add their additional data to the end of the packet, and this area may be
 * reused for different purposes as items travel down the chain.
 *
 * Execution occurs in several steps. First, we create zstream_queues for
 * every step that is to be performed in parallel.
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
void
zstream_chain_exec(zstream_chain_t chain, chain_attrs_t *attrs)
{
	struct chain_info chain_info = { .ci_chain = chain };

	chain_attrs_t backup_attrs = {0};
	chain_attrs = attrs ? attrs : &backup_attrs;

	validate_chain(&chain_info);

	int num_steps = chain_info.ci_num_steps;
	zstream_queue_t	*queues[num_steps] = {0};
	worker_context_t contexts[num_steps] = {0};
	pthread_t worker_threads[num_steps] = {0};
	int num_workers = 0;

	chain_info.ci_queues = queues;

	libraries_init();

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
				.qp_item_size	 = ci->ci_item_size,
				.qp_batch_budget = ci->cs_parallel.batch_budget,
				.qp_queue_length = ci->cs_parallel.queue_length,
				.qp_context	 = ci->cs_context
			};
			queues[i] = zstream_queue_create(&queue_params);
		}
	}

	/* Create worker contexts and assign step ranges to workers */
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
			(chain_worker *)zstream_chain_worker,
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
				if (done) {
					(void) step->cs_serial.process(NULL,
					    step->cs_context);
				} else {
					disposition_t dispo =
					    step->cs_serial.process(buffer,
					    step->cs_context);
					if (dispo == D_EOF) {
						VERIFY0(i);
						done = B_TRUE;
					} else if (dispo == D_DROP) {
						break;
					}
				}
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
			chain_step_t *step = &ci->ci_chain[i]
			if (step->cs_type == CS_SERIAL) {
				if (done) {
					(void) step->cs_serial.process(NULL,
					    step->cs_context);
				} else {
					disposition_t dispo =
					    step->cs_serial.process(buffer,
					    step->cs_context);
					if (dispo == D_EOF) {
						VERIFY0(i);
						done = B_TRUE;
					} else if (dispo == D_DROP) {
						break;
					}
				}
			} else if (!done) {
				size_t cost = step->cs_parallel.cost(buffer,
				    step->cs_context);
				if (cost > 0) {
					step->cs_parallel.process(buffer,
					    step->cs_context);
				}
			}
		}
	}
}
