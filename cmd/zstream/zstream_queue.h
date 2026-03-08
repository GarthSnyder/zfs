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

/*
 * REVIEW: Include guard must come before #includes, otherwise the includes
 * are processed on every inclusion even when the guard is already set.
 * See zstream_util.h for the correct pattern.
 *
 * REVIEW: <sys/dmu.h>, <sys/zio_checksum.h>, <sys/zfs_ioctl.h>, and
 * <sys/fs/zfs.h> are not used by this header. The only types needed are
 * boolean_t (from <sys/types.h>) and size_t (from <stddef.h>). Replace
 * these heavy includes with the minimal set. Suggested fix:
 *
 *     #ifndef _ZSTREAM_QUEUE_H
 *     #define _ZSTREAM_QUEUE_H
 *     #include <stddef.h>
 *     #include <sys/types.h>
 */
#include <stdint.h>
#include <sys/dmu.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>

#ifndef _ZSTREAM_QUEUE_H
#define _ZSTREAM_QUEUE_H

#ifdef  __cplusplus
extern "C" {
#endif

/*
 * REVIEW: Leading space on the opening comment delimiter (line below) is
 * inconsistent with OpenZFS style. The "/*" should be flush to column 1.
 */
 /*
  * This is a generalized implementation of multithreaded, FIFO work queues.
  * The order guarantee applies only to enqueueing and dequeueing. Work on
  * individual items can occur in any order, although the implementation
  * generally starts work in FIFO order as well.
  *
  * Callers define a fixed item size to be used by each queue and supply two
  * thread-safe functions that 1) estimate individual items' processing cost
  * and 2) perform the actual processing. The queue treats items as black
  * boxes, so processing functions can modify them as desired.
  *
  * The cost function assigns a size_t cost that estimates the amount of
  * work needed to process an item. For operations like hashing and data
  * compression, the natural cost is typically input buffer length. The cost
  * function is run as items enter the queue, so it's single-threaded and
  * should return a value promptly. If cost estimation is important and
  * expensive, use a separate queue to implement it.
  *
  * It's expected that only a subset of input items will require processing.
  * If an item's cost is 0, it is fast-tracked and never presented to the
  * processing function.
  *
  * All queues share a single thread pool that is managed to avoid
  * contention. Threads are allocated to queues dynamically according to
  * where work is available. When multiple queues have work, threads are
  * allocated among them stochastically with an eye toward preventing
  * pipeline stalls.
 */

#define MAX_BATCH 16	/* The most items that can be claimed at once */

typedef void queue_item;

struct zstream_queue;
typedef struct zstream_queue zstream_queue_t;

/*
 * Required signatures that cost and processing functions must conform to.
 */
typedef void
zq_process_item_f(queue_item *item, void *context);

typedef size_t
zq_estimate_cost_f(queue_item *item, void *context);

/*
 * Create a queue. Must be called before enqueue or dequeue.
 *
 * Threading granularity is specified as a per-batch budget that is set for
 * each queue in the same units used for item costs. Threads claim items
 * until the budget is met, there are no more items available, or MAX_BATCH
 * items have been claimed. When claiming items to work on, threads never
 * block waiting for additional work to arrive. They start work as quickly
 * as possible even if the budget has not been reached.
 *
 * The zq_context field is passed to the cost and processing functions and
 * is not examined by the queue itself.
 */

typedef struct {
	zq_process_item_f	*qp_process;
	zq_estimate_cost_f	*qp_cost;
	/* REVIEW: Alignment uses spaces; other fields use tabs. Use tabs. */
	void                    *qp_context;
	size_t			qp_item_size;
	size_t			qp_batch_budget;
	size_t			qp_queue_length;
} zq_params_t;

zstream_queue_t *
zstream_queue_create(zq_params_t *params);

/*
 * Submit a work item. The call blocks if the input queue is full. The work
 * item struct is shallow-copied into the queue and after zstream_enqueue
 * returns may be reused by the caller.
 */
void
zstream_enqueue(zstream_queue_t *queue, queue_item *item);

/*
 * Retrieve a completed work item. The caller must provide a buffer into
 * which the dequeued item is shallow-copied. Items are returned in the same
 * order they were submitted. If the next unit is not yet ready, this call
 * will block.
 *
 * If zstream_dequeue returns B_FALSE, the stream is complete. The returned
 * item is not valid and no further calls may be made.
 */
boolean_t
zstream_dequeue(zstream_queue_t *queue, queue_item *item);

/*
 * Declare that all items have been submitted. The queue will continue to
 * function normally for dequeuers and worker threads until zstream_dequeue()
 * returns B_FALSE, at which point the queue will be destroyed.
 */
void
zstream_queue_fini(zstream_queue_t *queue);

#ifdef  __cplusplus
}
#endif

#endif  /* _ZSTREAM_QUEUE_H */
