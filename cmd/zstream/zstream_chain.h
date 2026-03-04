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

#include "zstream_queue.h"
#include <stddef.h>
#include <stdint.h>

#ifndef _ZSTREAM_CHAIN_H
#define _ZSTREAM_CHAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A chain is a processing pipeline that runs on packets of data. The point
 * of this construct is to
 *
 *   * Separate processing steps into small, discrete modules
 *   * Facilitate component reuse (e.g., checksum validation, I/O)
 *   * Convert pipelines to a simple, declarative form
 *   * Handle marshalling of data as it passes down the pipeline
 *   * Smooth the interfaces between sequential and parallel processing
 *
 * Steps in a chain are defined by instances of the chain_step_t struct
 * below. Steps can be declared to execute either serially or in parallel.
 * Parallel steps are executed concurrently as outlined in zstream_queue.h,
 * and the parameters needed to set up the queue are included in the
 * chain_step_t struct.
 *
 * Both serial and parallel steps can pass an arbitrary (void *) pointer to
 * the processing function as a second argument. This context can be used to
 * maintain state between packets. (However, execution order is not
 * guaranteed in concurrent queues.)
 *
 * Input and output packet sizes are declared for each step. The buffer
 * containing the input packet passed to a processing function is large
 * enough to accommodate the declared output size. The processing function
 * should modify the buffer in place.
 *
 * The processing function for a serial step should normally return B_TRUE,
 * but it can return B_FALSE to indicate that no more data will be
 * forthcoming. Only the first step in a chain should use this feature,
 * however.
 *
 * Serial functions are called with a NULL packet when the end of the stream
 * passes by them. Since parallel functions may be called in any order, they
 * have no concept of "end of stream" and do not receive this notification.
 */

#define CA_BYTESWAPPED		(1ULL << 0)
#define CA_DEDUPED		(1ULL << 1)

#define CA_VERBOSE		(1ULL << 11)
#define CA_VERY_VERBOSE		(1ULL << 12)
#define CA_DUMP_OFFSETS		(1ULL << 13)
#define CA_DUMP_DATA		(1ULL << 14)
#define CA_IGNORE_CKSUMS	(1ULL << 15)

#define CHAIN_TERMINATOR ((chain_step_t) { .cs_type = CS_TERMINATE})
#define NULL_STACK	 ((zstream_chain_t) { CHAIN_TERMINATOR })

typedef struct chain_attrs {
	uint64_t	ca_flags;
	off_t		ca_offset;
} *chain_attrs_t;

typedef boolean_t
zc_serial_process_f(void *item, void *context, chain_attrs_t chain);

typedef enum { CS_SERIAL, CS_PARALLEL, CS_TERMINATE } step_type_t;

typedef struct chain_step {
		step_type_t		cs_type;
		size_t			cs_in_size;
		size_t 			cs_out_size;
		void			*cs_context;
    union {
	struct serial_config {
		zc_serial_process_f	*css_process;
	} cs_serial;
	struct parallel_config {
		size_t 			csp_queue_length;
		size_t 			csp_batch_budget;
		zq_estimate_cost_f	*csp_cost;
		zq_process_item_f	*csp_process;
	} cs_parallel;
    };
} chain_step_t;

typedef chain_step_t zstream_chain_t[];

/*
 * Serialize chain execution for debugging. This option forces all chains to
 * execute in a completely nonconcurrent and record-sequential fashion.
 * There are no worker threads, and each record traverses the entire chain
 * before execution returns to the head of the chain.
 *
 * Serialized execution should be logically identical to normal execution.
 * If it does not yield identical results, there's a problem somewhere.
 */
extern boolean_t serialize_chains;

/*
 * Execute a chain. This function returns once execution is complete. The
 * individual chain steps are unmodified and may be reused.
 */
void
zstream_chain_exec(zstream_chain_t chain, chain_attrs_t attrs);

chain_step_t
serial_null_step(void);

#ifdef __cplusplus
}
#endif

#endif	/* _ZSTREAM_CHAIN_H */
