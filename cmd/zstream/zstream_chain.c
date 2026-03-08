#include "zstream_chain.h"
#include "zstream_io.h"
#include <pthread.h>

/*
 * Execute a chain of processing steps, some parallel and some serial.
 *
 * For simplicity, we normalize the chain packet size to that of the largest
 * output of any step. Packets with data beyond the base drr_record_t should
 * add their additional data to the end of the packet, and this area may be
 * reused for different purposes as packets travel down the chain.
 *
 * Execution is straightforward. First, zstream_queues are created for every
 * step that is to be performed in parallel.
 *
 * Second, service threads are spawned for sections of the chain. There is
 * only one kind of service thread, and the only information needed by the
 * thread (other than access to the chain itself) is the range of steps it
 * is responsible for.
 *
 * One thread is assigned to every contiguous sequence of serial steps, plus
 * the parallel steps on either side, if any. Adjacent parallel steps also
 * receive a worker; this is just a special case of the same general
 * pattern, with the serial portion consisting of zero steps.
 *
 * Parallel steps are double-covered, which is the intended behavior. If a
 * worker's domain begins with a parallel step, it dequeues items from the
 * associated queue. If it ends with a parallel step, it submits items to
 * that queue.
 *
 * No thread management is required for the chain's worker threads, other
 * than operations implicit in their calls into the parallel queues.
 */

typedef struct chain_info {
	chain_step_t	*ci_chain;
	int		ci_num_steps;
	zstream_queue_t	**ci_queues;	/* Sparse */
	size_t		ci_item_size;
	chain_attrs_t	ci_attrs;
} *chain_info_t;

typedef struct {
	chain_info_t	wc_chain_info;
	int		wc_first;	/* Responsibility region */
	int		wc_last;
} worker_context_t;

typedef void *pthread_worker(void *);

static void *
zstream_chain_worker(worker_context_t *context);

static void
chain_exec_serialized(chain_info_t chain);

boolean_t serialize_chains = B_FALSE;

chain_step_t
serial_null_step() {
	return (chain_step_t) {
		.cs_type = CS_SERIAL,
		.cs_in_size = 0,
		.cs_out_size = 0,
		.cs_context = NULL,
		.cs_serial = {
			.css_process = NULL,
		}
	};
}

void
zstream_chain_exec(zstream_chain_t chain, chain_attrs_t attrs)
{
	int num_steps = 0;

	while (chain[num_steps].cs_type != CS_TERMINATE) num_steps++;

	zstream_queue_t		*queues[num_steps] = {};
	size_t			max_size = 0;
	worker_context_t	contexts[num_steps] = {};
	pthread_t		worker_threads[num_steps] = {};
	int			num_workers = 0;

	VERIFY3U(num_steps, >, 0);
	if (chain[num_steps-1].cs_type == CS_PARALLEL ||
		chain[0].cs_type == CS_PARALLEL)
	{
		fprintf(stderr, "A zstream_chain cannot start or end "
			"with a parallel step.");
		exit(1);
	}

	/* Double check expected input and output packet sizes */
	for (int i = 0; i < num_steps; i++) {
		if (chain[i].cs_out_size > max_size) {
			max_size = chain[i].cs_out_size;
		}
		if (i > 0 && chain[i].cs_in_size && chain[i-1].cs_out_size &&
			chain[i].cs_in_size != chain[i-1].cs_out_size)
		{
			fprintf(stderr, "Warning: chain items %d and %d have "
				"mismatched packet sizes\n", i-1, i);
		}
	}

	struct chain_info chain_info = {
		.ci_chain = chain,
		.ci_num_steps = num_steps,
		.ci_queues = queues,
		.ci_item_size = max_size,
		.ci_attrs = attrs
	};
	if (serialize_chains) {
		chain_exec_serialized(&chain_info);
		return;
	}

	/* Create parallel queues */
	for (int i = 0; i < num_steps; i++) {
	    if (chain[i].cs_type == CS_PARALLEL) {
		 chain_step_t *ci = &chain[i];
		 zq_params_t queue_params = {
	 		.qp_process = ci->cs_parallel.csp_process,
	 		.qp_estimate_cost = ci->cs_parallel.csp_cost,
	 		.qp_item_size = max_size,
	 		.qp_batch_budget = ci->cs_parallel.csp_batch_budget,
	 		.qp_queue_length = ci->cs_parallel.csp_queue_length,
	 		.qp_context = ci->cs_context
	 	};
	 	queues[i] = zstream_queue_create(&queue_params);
	    }
	}

	/* Create worker context bundles and assign step ranges */
	int last = 0;
	for (int first = 0; first < num_steps - 1; first = last) {
		for (last = first + 1; last < num_steps &&
			chain[last].cs_type != CS_PARALLEL; last++) {}
		last = MIN(last, num_steps - 1);
		contexts[num_workers].wc_chain_info = &chain_info;
		contexts[num_workers].wc_first = first;
		contexts[num_workers].wc_last = last;
		num_workers++;
	}

	/* Create threads */
	for (int i = 0; i < num_workers; i++) {
		char buff[32];
		assert(pthread_create(&worker_threads[i], NULL,
			(pthread_worker *)zstream_chain_worker,
			&contexts[i]) == 0);
		sprintf(buff, "chain-%d", i);
		pthread_setname_np(worker_threads[i], buff);
	}

	/* Await completion */
	for (int i = 0; i < num_workers; i++) {
		assert(pthread_join(worker_threads[i], NULL) == 0);
	}
}

static void *
zstream_chain_worker(worker_context_t *context)
{
	chain_info_t ci = context->wc_chain_info;
	uint8_t buffer[ci->ci_item_size];
	boolean_t done = B_FALSE;

	while (!done) {
	    for (int i = context->wc_first; i <= context->wc_last; i++) {
	    	chain_step_t *step = &ci->ci_chain[i];
	    	zstream_queue_t *queue = ci->ci_queues[i];
		if (step->cs_type == CS_SERIAL) {
		    done = !step->cs_serial.css_process(done ? NULL : buffer,
			step->cs_context, ci->ci_attrs) || done;
		} else if (i == context->wc_first) {
			done = done || !zstream_dequeue(queue, buffer);
		} else if (done) {
			zstream_queue_fini(queue);
		} else {
			zstream_enqueue(queue, buffer);
		}
	    }
	}
	return NULL;
}

/*
 * Execute a chain linearly, without queues and without multithreading. For
 * debugging. This option is triggered by setting serialize_chains to B_TRUE.
 */
static void
chain_exec_serialized(chain_info_t ci)
{
	uint8_t buffer[ci->ci_item_size];
	boolean_t done = B_FALSE;

	while (!done) {
	    for (int i = 0; i < ci->ci_num_steps; i++) {
		if (ci->ci_chain[i].cs_type == CS_SERIAL) {
			uint8_t *arg = done ? NULL : buffer;
			done = !ci->ci_chain[i].cs_serial.css_process(arg,
				ci->ci_chain[i].cs_context, ci->ci_attrs) ||
				done;
		} else if (!done) {
			size_t cost = ci->ci_chain[i].cs_parallel.csp_cost(
				buffer, ci->ci_chain[i].cs_context);
			if (cost > 0) {
				ci->ci_chain[i].cs_parallel.csp_process(buffer,
					ci->ci_chain[i].cs_context);
			}
		}
	    }
	}
}

