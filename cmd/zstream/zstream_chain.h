#import "zstream_queue.h"

/*
 * A chain is a sequence of processing steps that are run on packets of
 * data. Data packet size must be the same for all packets at a given step,
 * but the packet size can vary along the chain as steps add or remove data.
 *
 * Steps in a chain are defined by instances of the chain_step_t struct
 * below. Steps may be declared to execute either serially or in parallel.
 * Parallel steps are executed concurrently as outlined in zstream_queue.h,
 * and the parameters needed to set up the queue are included in the
 * chain_step_t struct. Serial steps have the option to pass an arbitrary
 * (void *) pointer to the processing function as a second argument. This
 * context can be used to maintain state between packets.
 *
 * Both serial and parallel steps must declare the size of their output
 * packets. The buffer containing the input packet passed to a processing
 * function is guaranteed to be large enough to accommodate the stated
 * output size, and the processing function should write its output to the
 * same buffer.
 *
 * The processing function for a serial step should normally return B_TRUE,
 * but it can return B_FALSE to indicate that no more data will be
 * forthcoming. Only the first step in a chain should use this feature.
 *
 * Serial functions are called with a NULL packet when the end of the stream
 * passes by them. Since parallel functions may be called in any order, they
 * have no concept of "end of stream" and do not receive this notification.
 */

typedef enum { CS_SERIAL, CS_PARALLEL } step_type_t;

typedef boolean_t
zc_process_item_f(void *item, void *context);

typedef struct {
			step_type_t		cs_type;
			size_t 			cs_out_size;
	union {
		struct {
			zc_process_item_f	*css_process;
			void			*css_context;
		} serial;
		struct {
			size_t 			csp_queue_length;
			size_t 			csp_batch_budget;
			zq_estimate_cost_f	*csp_cost;
			zq_process_item_f	*csp_process;
		} parallel;
	}
} chain_step_t;

typedef chain_step_t zstream_chain_t[];

/*
 * Execute a chain. This function returns once execution is complete. The
 * individual chain steps are unmodified and may be reused.
 */
void
zstream_chain_exec(zstream_chain_t chain, int num_steps);


