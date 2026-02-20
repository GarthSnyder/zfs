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
  * This is a generalized implementation of multithreaded, FIFO work queues.
  * I'm calling them queues, but there is no inherent order of work inside
  * the queue. The only guarantee is that completed items will emerge from
  * the queue in the same order they entered. The implementation broadly
  * tries to work on items in order, but that is not guaranteed.
  *
  * Callers define a fixed item size to be used by each queue and define
  * thread-safe functions that estimate individual items' processing cost
  * and perform the actual processing. The queue treats items as black
  * boxes, so processing functions can modify them freely.
  *
  * The cost estimation function assigns a size_t cost that represents the
  * amount of work needed to process an item. For activities like hashing
  * and data compression, the natural cost is typically input buffer length.
  * This function is run as items enter the queue, so it's single-threaded
  * and should return a value promptly. If cost estimation is important and
  * expensive, use a separate queue to implement it.
  *
  * It's expected that only a subset of input items will require processing.
  * If an item's cost is zero, it is fast-tracked and never presented to the
  * processing function.
  *
  * Threading granularity is specified by a per-batch budget that is set for
  * each queue in the same units used for item costs. Threads claim items
  * until the budget is met, there are no more items available, or MAX_BATCH
  * items have been claimed. When collecting items, threads never wait for
  * additional work to arrive. They start work as quickly as possible even
  * if the budget has not been reached.
  *
  * All queues share a single thread pool that is managed to avoid
  * contention. Threads are allocated to queues dynamically according to
  * where work is available. When multiple queues have work, threads are
  * allocated among them stochastically with an eye toward preventing
  * pipeline stalls.
 */

#define MAX_BATCH 16	/* Most items that can be claimed at once */

typedef void queue_item;

struct zstream_queue;
typedef struct zstream_queue *zstream_queue_t;

typedef void
zq_process_item_f(queue_item *item);

typedef size_t
zq_estimate_cost_f(queue_item *item);

/*
 * Create a queue. Must be called before enqueue or dequeue.
 *
 * A target budget can be set to allow threads to claim multiple items for
 * processing at once for processing, up to a maximum of 16 records. This
 * measure can be helpful when multithreading overhead is significant in
 * comparison to work time. If the qp_batch_budget is zero, items are always
 * processed individually.
 */

typedef struct {
	zq_process_item_f	*qp_process;
	zq_estimate_cost_f	*qp_estimate_cost;
	size_t			qp_item_size;
	size_t			qp_batch_budget;
	size_t			qp_queue_length;
} zq_params_t;

zstream_queue_t
zstream_queue_create(zq_params_t *params);

/*
 * Submit a work item. The call blocks if the input queue is full. The work item
 * struct is shallow-copied into the queue and can be reused by the caller.
 *
 * queue	queue in which to place the item
 * item		pointer to the item struct (caller-defined)
 */
void
zstream_enqueue(zstream_queue_t queue, queue_item *item);

/*
 * Retrieve a completed work item. The caller must provide a buffer into which
 * the dequeued item is copied. Items are returned in the same order they were
 * submitted. If the next unit is not yet ready, this call will block.
 *
 * If zstream_dequeue returns B_FALSE, the stream is complete. The returned item
 * is not valid, and no further calls may be made.
 */
boolean_t
zstream_dequeue(zstream_queue_t queue, queue_item *item);

/*
 * Declare that all items have been submitted. The queue will continue to
 * function normally for dequeuers and worker threads until zstream_dequeue()
 * returns B_FALSE, at which point the queue will be destroyed.
 */
void
zstream_queue_fini(zstream_queue_t queue);

#ifdef  __cplusplus
}
#endif

#endif  /* _ZSTREAM_QUEUE_H */
