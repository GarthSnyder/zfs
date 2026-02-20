#include "zstream_chain.h"

/*
 * Execute a chain
 *
 * For simplicity, we normalize the chain packet size to that of the largest output
 * of any step. Packets with data beyond the base drr_record_t should add their
 * additional data to the end of the packet, and this area may be reused for
 * different purposes as packets travel down the chain.
 *
 * Internally, chain execution is straightforward. First, zstream_queues are created
 * for every step that is to be performed in parallel. These queues manage execution
 * of the steps on their own, but they need chain workers to enqueue and dequeue
 * packets to and from them.
 *
 * Second, service threads are spawned for sections of the chain. There is only one
 * kind of service thread, and the only information needed by the thread (other than
 * access to the chain itself) is the range of steps it is responsible for.
 *
 * One thread is assigned to every contiguous sequence of serial steps, plus the
 * parallel steps on either side, if any. Adjacent parallel steps also receive a
 * worker; this is just a special case of the same general pattern, with the serial
 * portion consisting of zero steps.
 *
 * Parallel steps are double-covered, which is the intended behavior. If a worker's
 * domain begins with a parallel step, it dequeues items from the associated queue.
 * If it ends with a parallel step, it submits items to that queue.
 *
 * No thread synchronization of any kind is required for the chain's worker threads,
 * other than that implicit in their calls into the parallel queues. By design, there
 * is no possibility of overlap or interference.
 */

typedef struct chain_info {
	chain_step_t	*ci_chain;
	int		ci_num_steps;
	zstream_queue_t	*ci_queues;
	size_t		ci_item_size;
	chain_attrs_t	ci_attrs;
} *chain_info_t;

typedef struct {
	chain_info_t	wc_chain_info;
	int		wc_first;
	int		wc_last;
} worker_context_t;

typedef void *pthread_worker(void *);

static void *
zstream_chain_worker(worker_context_t *context);

size_t
payload_size_as_cost(drr_packet_t *drr);

void
zstream_chain_exec(zstream_chain_t chain, chain_attrs_t attrs,
	int num_steps)
{
	zstream_queue_t		queues[num_steps];
	size_t			max_size = 0;
	worker_context_t	contexts[num_steps];
	pthread_t		worker_threads[num_steps];
	int			num_workers = 0;

	assert(num_steps);
	if (chain[num_steps-1].cs_type == CS_PARALLEL ||
		chain[0].cs_type == CS_PARALLEL)
	{
		fprintf(stderr, "A zstream_chain cannot start or end "
			"with a parallel step.");
		exit(1);
	}
	/* Find largest output size */
	for (int i = 0; i < num_steps; i++) {
		if (chain[i].cs_out_size > max_size) {
			max_size = chain[i].cs_out_size;
		}
	}
	/* Create parallel queues */
	for (int i = 0; i < num_steps; i++) {
		if (chain[i].cs_type == CS_PARALLEL) {
			chain_step_t *ci = &chain[i];
			zq_params_t queue_params = {
				.qp_process = ci->parallel.csp_process,
				.qp_estimate_cost = ci->parallel.csp_cost,
				.qp_item_size = max_size,
				.qp_batch_budget = ci->parallel.csp_batch_budget,
				.qp_queue_length = ci->parallel.csp_queue_length
			};
			/*
			 * Add payload length as default cost metric if no
			 * other cost function is assigned.
			 */
			if (!queue_params.qp_estimate_cost) {
				queue_params.qp_estimate_cost =
				    (zq_estimate_cost_f *)payload_size_as_cost;
			}
			queues[i] = zstream_queue_create(&queue_params);
		}
	}

	/* Create worker context bundles and assign step ranges */
	struct chain_info chain_info = {
		.ci_chain = chain,
		.ci_num_steps = num_steps,
		.ci_queues = queues,
		.ci_item_size = max_size,
		.ci_attrs = attrs
	};
	int last = 0;
	for (int first = 0; first < num_steps; first = last) {
		contexts[num_workers].wc_chain_info = &chain_info;
		contexts[num_workers].wc_first = first;
		for (last = first + 1; last < num_steps &&
			chain[last].cs_type != CS_PARALLEL; last++) {}
		if (last >= num_steps) {
			/* End of chain */
			contexts[num_workers].wc_last = last - 1;
			break;
		} else {
			contexts[num_workers].wc_last = last;
		}
		num_workers++;
	}

	/* Create and monitor threads */
	for (int i = 0; i < num_workers; i++) {
		assert(pthread_create(&worker_threads[i], NULL,
			(pthread_worker *)zstream_chain_worker,
			&contexts[i]) == 0);
	}
	for (int i = 0; i < num_workers; i++) {
		assert(pthread_join(worker_threads[i], NULL) == 0);
	}
}

static void *
zstream_chain_worker(worker_context_t *context) {
	chain_info_t ci = context->wc_chain_info;
	uint8_t buffer[ci->ci_item_size];
	boolean_t done = B_FALSE;
	boolean_t more;
	repeat: for (int i = context->wc_first; i <= context->wc_last; i++) {
		if (ci->ci_chain[i].cs_type == CS_SERIAL) {
			uint8_t *arg = done ? NULL : buffer;
			done = done || ci->ci_chain[i].serial.css_process(arg,
				ci->ci_chain[i].serial.css_context,
				ci->ci_attrs);
		} else if (i == context->wc_first) {
			more = zstream_dequeue(ci->ci_queues[i], buffer);
			done = done || !more;
		} else if (done) {
			zstream_queue_fini(ci->ci_queues[i]);
		} else {
			zstream_enqueue(ci->ci_queues[i], buffer);
		}
	}
	if (done) {
		return (NULL);
	} else {
		goto repeat;
	}
}

size_t
payload_size_as_cost(drr_packet_t *packet) {
	return (size_t)packet->dp_payload_size;
}

