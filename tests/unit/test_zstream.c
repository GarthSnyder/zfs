// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

/*
 * Runs the "zstream selftest" suites (cmd/zstream/zstream_selftest*.c)
 * under munit, so that "make unit" and sanitizer builds exercise them.
 * Each zstream module is one munit test, parameterized by the name of the
 * selftest case to run, so each case runs in its own forked process.
 *
 * The queue cases run once with the default worker pool and once with a
 * single worker thread, as the ZTS wrapper does. The pool size is fixed
 * once the first queue exists, so under --no-fork only the first size
 * requested is tested and the others are skipped.
 *
 * The selftest seed is derived from munit's, so --seed replays a run. The
 * derived seed is logged for use with "zstream selftest -s".
 */

#include <libspl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zstream.h"
#include "zstream_backtrace.h"
#include "zstream_queue.h"
#include "zstream_selftest.h"
#include "zstream_util.h"

#include "unit.h"

/*
 * The process setup that zstream's main() and zstream_do_selftest() do.
 * With munit's default forking, this runs in each test's child process.
 */
static void
selftest_process_init(void)
{
	static boolean_t initialized = B_FALSE;
	sigset_t mask;

	if (initialized)
		return;
	initialized = B_TRUE;

	/* As in zstream.c, this must precede the creation of any thread */
	safe_pthread_sigmask(SIG_SETMASK, NULL, &mask);
	sigaddset(&mask, WATCHDOG_SIGNAL);
	sigdelset(&mask, THREAD_BACKTRACE_SIGNAL);
	safe_pthread_sigmask(SIG_SETMASK, &mask, NULL);

	libspl_init();
	watchdog_init();
}

static MunitResult
run_selftest_case(const MunitParameter params[], const test_case_t *cases)
{
	const char *name = munit_parameters_get(params, "case");
	const test_case_t *tc = cases;

	while (tc->tc_name != NULL && strcmp(tc->tc_name, name) != 0)
		tc++;
	if (tc->tc_name == NULL)
		munit_errorf("no selftest case named '%s'", name);

	selftest_process_init();

	/* munit reseeds its PRNG before each test, so this is per-run */
	selftest_seed = unit_rand_uint64();
	munit_logf(MUNIT_LOG_INFO, "replay with: zstream selftest -s 0x%jx",
	    (uintmax_t)selftest_seed);

	watchdog_arm();
	tc->tc_func();
	watchdog_disarm();

	return (MUNIT_OK);
}

static MunitResult
test_queue(const MunitParameter params[], void *data)
{
	static const char *pool_size = NULL;
	const char *threads = munit_parameters_get(params, "threads");

	(void) data;

	/* zstream_queue_set_num_threads() can start the dispatch thread */
	selftest_process_init();

	if (pool_size == NULL) {
		pool_size = threads;
		if (strcmp(threads, "default") != 0)
			zstream_queue_set_num_threads(atoi(threads));
	} else if (strcmp(threads, pool_size) != 0) {
		munit_logf(MUNIT_LOG_INFO, "worker pool size is already %s",
		    pool_size);
		return (MUNIT_SKIP);
	}

	return (run_selftest_case(params, selftest_queue_cases));
}

static MunitResult
test_heap(const MunitParameter params[], void *data)
{
	(void) data;
	return (run_selftest_case(params, selftest_heap_cases));
}

static MunitResult
test_alloc(const MunitParameter params[], void *data)
{
	(void) data;
	return (run_selftest_case(params, selftest_alloc_cases));
}

static MunitResult
test_hash(const MunitParameter params[], void *data)
{
	(void) data;
	return (run_selftest_case(params, selftest_hash_cases));
}

/* The values for each "case" parameter are filled in by main() */
static MunitParameterEnum queue_params[] = {
	UNIT_PARAM("threads", "default", "1"),
	{ (char *)"case", NULL },
	{ NULL, NULL },
};

static MunitParameterEnum heap_params[] = {
	{ (char *)"case", NULL },
	{ NULL, NULL },
};

static MunitParameterEnum alloc_params[] = {
	{ (char *)"case", NULL },
	{ NULL, NULL },
};

static MunitParameterEnum hash_params[] = {
	{ (char *)"case", NULL },
	{ NULL, NULL },
};

static const MunitTest zstream_tests[] = {
	UNIT_TEST("queue",	test_queue,	queue_params),
	UNIT_TEST("heap",	test_heap,	heap_params),
	UNIT_TEST("alloc",	test_alloc,	alloc_params),
	UNIT_TEST("hash",	test_hash,	hash_params),
	{ 0 },
};

static const MunitSuite zstream_test_suite = {
	"zstream.",
	zstream_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE,
};

/*
 * Point the "case" parameter of a test at the names of its selftest cases
 */
static void
set_case_names(MunitParameterEnum *params, const test_case_t *cases)
{
	size_t n = 0;

	while (cases[n].tc_name != NULL)
		n++;
	char **names = safe_calloc((n + 1) * sizeof (char *));
	for (size_t i = 0; i < n; i++)
		names[i] = (char *)cases[i].tc_name;

	while (strcmp(params->name, "case") != 0)
		params++;
	params->values = names;
}

int
main(int argc, char **argv)
{
	set_case_names(queue_params, selftest_queue_cases);
	set_case_names(heap_params, selftest_heap_cases);
	set_case_names(alloc_params, selftest_alloc_cases);
	set_case_names(hash_params, selftest_hash_cases);

	return (munit_suite_main(&zstream_test_suite, NULL, argc, argv));
}
