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

#ifndef _ZSTREAM_CHAIN_H
#define _ZSTREAM_CHAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "zstream_queue.h"

/*
 * A chain is a linear series of steps that process packets of data. The
 * purpose of this construct is to:
 *
 *   - Reduce code duplication
 *   - Separate processing into small, logically distinct steps
 *   - Separate pipeline management from functional processing
 *   - Facilitate component reuse (checksum validation, I/O, etc.)
 *   - Automatically marshal data as it passes down the pipeline
 *   - Interface automatically between sequential and parallel processing
 *   - No, this was not written by an LLM, despite being a list
 *
 * Some terms:
 *
 * **STEP** - A chain_step_t struct that represents a packet-processing
 * module. Modules generally define a function named serial_* or
 * parallel_* that produces a chain_step_t which can be incorporated
 * directly into a chain.
 *
 * **CHAIN** - An array of chain_step_t's. It's just data, so you can create
 * the array however you like. But normally you'd just declare the whole
 * thing:
 *
 *	zstream_chain_t dump_chain = {
 *		serial_read_stream(infile),
 *		parallel_calc_fletcher4(f4_queue_length),
 *		serial_validate_fletcher4(),
 *		serial_byteswap(),
 *		serial_validate_records(),
 *		serial_dump_records(&dump_args),
 *		chain_terminator()
 *	}
 *
 * Or more succinctly:
 *
 *	zstream_chain_t recompress_chain = {
 *		STANDARD_INPUT_STACK(infile, 1024),
 *		parallel_decompress_writes(&spec),
 *		parallel_compress_writes(&spec),
 *		STANDARD_OUTPUT_STACK(NULL, 512)
 *	};
 *
 * Chains must be terminated by a step of type CS_TERMINATE.
 *
 * **ITEM** - The data packets that flow through a chain. Each step accepts
 * items of one size and emits items of another size, which may be
 * smaller, larger, or the same size. In the context of zstream, items
 * will generally be structs that start with a drr_packet_t (defined in
 * zstream_io.h) and may include additional module-specific fields.
 *
 * **PROCESSING FUNCTION** - Each step names a processing function that
 * transforms a buffer of its input size to a buffer of its output size. The
 * transformation happens in place, in a single buffer provided by the
 * chain. (Parallel steps must also identify a cost function; see
 * zstream_queue.h.)
 *
 * The processing function for a serial step should return a boolean_t,
 * normally B_TRUE, but it can return B_FALSE to indicate that no more data
 * will be forthcoming. Only the first step in a chain should use this
 * feature, however.
 *
 * Serial functions are called with a NULL packet when the end of the
 * stream passes by them. Since parallel functions may be called in any
 * order, they have no concept of "end of stream" and do not receive this
 * notification.
 *
 * **CONTEXT** - An arbitrary void * that the chain passes along to the
 * processing function as an argument.
 *
 * **CHAIN ATTRIBUTES** - A set of flags available to all serial steps.
 */

/*
 * Chain attribute flags that describe the stream. The lower 32 bits are a
 * copy of the drr_versioninfo from the first DRR_BEGIN in the stream. The
 * next 8 bits are reserved for discovered characteristics such as
 * CA_BYTESWAPPED, and the remainder are for command-line options.
 */

#define CA_BYTESWAPPED		(1ULL << 0)	/* ca_attrs */

#define CA_VERBOSE		(1ULL << 0)	/* ca_command_opts */
#define CA_VERY_VERBOSE		(1ULL << 1)
#define CA_DUMP_OFFSETS		(1ULL << 2)
#define CA_DUMP_DATA		(1ULL << 3)
#define CA_IGNORE_CKSUMS	(1ULL << 4)
#define CA_DO_NOT_VALIDATE	(1ULL << 5)
#define CA_FORBID_DEDUP		(1ULL << 6)
#define CA_REQUIRE_DEDUP	(1ULL << 7)

#define OPTION_ENABLED(attrs, opt) (!!((attrs)->ca_command_opts & (opt)))
#define STREAM_HAS_FEATURE(attrs, feat) (!!((attrs)->ca_feature_flags & (feat)))
#define ATTR_IS_SET(attrs, attr) (!!((attrs)->ca_attrs & (attr)))

#define	ENABLE_OPTION(attrs, opt) ((attrs)->ca_command_opts |= (opt))
#define	SET_ATTR(attrs, atr) ((attrs)->ca_attrs |= (atr))

typedef struct {
	uint64_t	ca_feature_flags;	/* From drr_versioninfo */
	uint64_t	ca_attrs;		/* Discovered attributes */
	uint64_t	ca_command_opts;
} chain_attrs_t;

/*
 * See zstream_queue.h for function signatures used by parallel steps.
 */
typedef boolean_t
zc_serial_process_f(void *item, void *context, chain_attrs_t *chain);

typedef enum { CS_SERIAL, CS_PARALLEL, CS_TERMINATE } step_type_t;

typedef struct chain_step
{
	step_type_t	cs_type;
	size_t		cs_in_size;
	size_t		cs_out_size;
	void		*cs_context;
	union {
		struct {
			zc_serial_process_f	*process;      /* cs_serial */
		} cs_serial;
		struct {
			size_t			queue_length;  /* cs_parallel */
			size_t			batch_budget;
			zq_estimate_cost_f	*cost;
			zq_process_item_f	*process;
		} cs_parallel;
	};
} chain_step_t;

typedef chain_step_t zstream_chain_t[];

/*
 * Serialize chain execution for debugging. This option forces all chains to
 * execute nonconcurrently. There are no worker threads, and each item
 * traverses the entire chain before execution returns to the head.
 *
 * Serialized execution should be logically identical to normal execution.
 * If it does not yield identical results, there's a problem somewhere.
 */
extern boolean_t serialize_chains;

/*
 * Execute a chain. This function returns once execution is complete.
 */
void
zstream_chain_exec(zstream_chain_t chain, chain_attrs_t attrs);

chain_step_t
serial_null_step(void);

chain_step_t
chain_terminator(void);

#ifdef __cplusplus
}
#endif

#endif	/* _ZSTREAM_CHAIN_H */
