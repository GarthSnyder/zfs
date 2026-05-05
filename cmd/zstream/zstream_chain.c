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

#define MAX_CHAIN_LENGTH 32

chain_attrs_t *chain_attrs;

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

/*
 * Execute a chain of serial processing steps.
 *
 * For simplicity, we normalize the chain item size to that of the largest
 * output of any step. Packets with data beyond the base drr_record_t should
 * add their additional data to the end of the packet, and this area may be
 * reused for different purposes as items travel down the chain.
 *
 * Each item traverses the entire chain before the next item is read.
 */
void
zstream_chain_exec(zstream_chain_t chain, chain_attrs_t *attrs)
{
	int num_steps = 0;
	size_t packet_size = 0;
	chain_attrs_t backup_attrs = {0};

	chain_attrs = attrs ? attrs : &backup_attrs;

	while (chain[num_steps].cs_type != CS_TERMINATE) {
		if (num_steps > MAX_CHAIN_LENGTH) {
			errx(1, "unterminated zstream_chain");
		}
		packet_size = MAX(packet_size, chain[num_steps].cs_out_size);
		num_steps++;
	}
	VERIFY3U(num_steps, >, 0);

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
			warnx("note - chain steps %d and %d have "
			    "mismatched packet sizes", i - 1, i);
		}
	}

	libraries_init();

	uint8_t buffer[packet_size];
	boolean_t done = B_FALSE;

	while (!done) {
		for (int i = 0; i < num_steps; i++) {
			if (done) {
				(void) chain[i].cs_serial.process(NULL,
					chain[i].cs_context);
			} else {
				disposition_t dispo =
				    chain[i].cs_serial.process(buffer,
				    chain[i].cs_context);
				if (dispo == D_EOF) {
					done = B_TRUE;
				} else if (dispo == D_DROP) {
					break;
				}
			}
		}
	}

	libraries_fini();
}
