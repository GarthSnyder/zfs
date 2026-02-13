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
 *
 * This is a generalized implementation of multithreaded, FIFO work queues. Even
 * though I call them queues, there is no inherent order of work inside the
 * queue. The only guarantee is that completed items will emerge from the queue
 * in the same order they entered. The implementation broadly tries to work on
 * items in order, but that is not guaranteed.
 *
 * Callers define a fixed item size to be used by each queue and supply a
 * thread-safe function that processes a single item. The queue treats items as
 * black boxes, so processing functions can modify them freely.
 *
 * Callers assign each incoming item an integer cost that represents the amount
 * of work needed to process it. For activities like hashing and data
 * compression, think "input buffer length."
 *
 * It's expected that only a subset of input items will require processing. If
 * an item's cost is zero, it is fast-tracked and never presented to the
 * processing function.
 *
 * Threading granularity is specified by a per-batch budget that is set for each
 * channel in the same units used for item costs. Threads claim items until the
 * budget is met or there are no more items available. When collecting items,
 * threads never wait for additional work to arrive. They start work as quickly
 * as possible even if the budget has not been reached.
 *
 * All queues share a single thread pool that is managed to avoid contention.
 * Threads are allocated to queues dynamically according to where work is
 * available. When multiple queues have work, threads are allocated among them
 * in averagely-equal proportion.
 */

#pragma once

#include <stdint.h>
#include <sys/dmu.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>

typedef void queue_item;

struct zstream_queue;
typedef struct zstream_queue *zstream_queue;

typedef void
process_item_func(queue_item *item);

/*
 * Create a queue. Must be called before enqueue or dequeue.
 *
 * A target budget can be set to allow threads to claim multiple items for
 * processing at once for processing, up to a maximum of 16 records. This
 * measure can be helpful when multithreading overhead is significant in
 * comparison to work time. The batch size is based on the net amount of payload
 * data. If it's zero, records are processed individually (though concurrently).
 *
 * process		function that does actual processing of work items
 * item_size	sizeof(struct work_item) where work_item is caller-defined
 * batch_budget	target cost sum per thread dispatch loop, 0 for item-by-item
 * queue_length	number of items the queue can hold at once
 * max_threads	limit threads that can work on this queue concurrently
 * 				  0 == no specific limit == number of CPU cores
 */
zstream_queue zstream_queue_create(process_item_func process, size_t item_size,
	int batch_budget, uint64_t queue_length, int max_threads);

/*
 * Submit a work item. The call blocks if the input queue is full. The work item
 * struct is shallow-copied into the queue and can be reused by the caller.
 *
 * queue		queue in which to place the item
 * item			pointer to the item struct (caller-defined)
 * cost			expected amount of work to process this item (caller-defined)
 * 				  set to 0 if this item does not require processing
 */
void
zstream_enqueue(zstream_queue queue, queue_item *item, int cost);

/*
 * Retrieve a completed work item. The caller must provide a buffer into which
 * the dequeued item is copied. Items are returned in the same order they were
 * submitted. If the next unit is not yet ready, this call will block.
 *
 * If zstream_dequeue returns B_FALSE, the stream is complete. The returned item
 * is not valid, and no further calls may be made.
 */
boolean_t
zstream_dequeue(zstream_queue queue, queue_item *item);

/*
 * Declare that all work items have been submitted. The queue will continue to
 * function normally for dequeuers and worker threads until zstream_dequeue()
 * returns B_FALSE, at which point the queue will be destroyed.
 */
void
zstream_queue_fini(zstream_queue queue);

