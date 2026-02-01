// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/blake3.h>
#include "blake3_team.h"

#define	TEST_PASS	"\033[32mPASS\033[0m"
#define	TEST_FAIL	"\033[31mFAIL\033[0m"

static uint64_t tests_passed = 0;
static uint64_t tests_failed = 0;

/*
 * Generate random payload data
 */
static uint8_t *
generate_random_payload(uint64_t size)
{
	uint8_t *payload = malloc(size);
	if (payload == NULL) {
		fprintf(stderr, "Failed to allocate payload of size %lu\n",
		    size);
		exit(1);
	}

	for (uint64_t i = 0; i < size; i++) {
		payload[i] = (uint8_t)(rand() % 256);
	}

	return (payload);
}

/*
 * Compute Blake3 hash for verification
 */
static void
compute_blake3(uint8_t *payload, uint64_t size, zio_checksum_t *hash)
{
	BLAKE3_CTX ctx;
	Blake3_Init(&ctx);
	Blake3_Update(&ctx, payload, size);
	Blake3_Final(&ctx, (uint8_t *)hash);
}

/*
 * Compare two Blake3 hashes
 */
static bool
blake3_equal(zio_checksum_t *a, zio_checksum_t *b)
{
	return (memcmp(a, b, sizeof (zio_checksum_t)) == 0);
}

/*
 * Test 1: Basic single work unit submission and retrieval
 */
static void
test_single_unit(void)
{
	printf("Test 1: Single work unit... ");

	struct drr_work_unit *unit = malloc(sizeof (struct drr_work_unit));
	memset(unit, 0, sizeof (*unit));

	unit->payload_size = 4096;
	unit->payload = generate_random_payload(unit->payload_size);
	unit->sequence_num = 0;

	zio_checksum_t expected_hash;
	compute_blake3(unit->payload, unit->payload_size, &expected_hash);

	blake3_team_submit(unit);
	struct drr_work_unit *result = blake3_team_retrieve();

	if (result != unit) {
		printf("%s - Wrong unit returned\n", TEST_FAIL);
		tests_failed++;
		return;
	}

	if (!blake3_equal(&result->blake3, &expected_hash)) {
		printf("%s - Hash mismatch\n", TEST_FAIL);
		tests_failed++;
		return;
	}

	free(unit->payload);
	free(unit);

	printf("%s\n", TEST_PASS);
	tests_passed++;
}

/*
 * Test 2: Multiple work units with ordering verification
 */
static void
test_ordering(void)
{
	printf("Test 2: Ordering preservation... ");

	const int num_units = 100;
	struct drr_work_unit *units[num_units];
	zio_checksum_t expected_hashes[num_units];

	/*
	 * Create and submit work units with various sizes
	 */
	for (int i = 0; i < num_units; i++) {
		units[i] = malloc(sizeof (struct drr_work_unit));
		memset(units[i], 0, sizeof (*units[i]));

		/*
		 * Vary sizes between 512 bytes and 1MB
		 */
		units[i]->payload_size = 512 + (rand() % (1024 * 1024));
		units[i]->payload = generate_random_payload(
		    units[i]->payload_size);
		units[i]->sequence_num = i;

		compute_blake3(units[i]->payload, units[i]->payload_size,
		    &expected_hashes[i]);

		blake3_team_submit(units[i]);
	}

	/*
	 * Retrieve and verify ordering
	 */
	for (int i = 0; i < num_units; i++) {
		struct drr_work_unit *result = blake3_team_retrieve();

		if (result->sequence_num != (uint64_t)i) {
			printf("%s - Out of order: expected %d, got %lu\n",
			    TEST_FAIL, i, result->sequence_num);
			tests_failed++;
			return;
		}

		if (!blake3_equal(&result->blake3, &expected_hashes[i])) {
			printf("%s - Hash mismatch for unit %d\n",
			    TEST_FAIL, i);
			tests_failed++;
			return;
		}
	}

	/*
	 * Cleanup
	 */
	for (int i = 0; i < num_units; i++) {
		free(units[i]->payload);
		free(units[i]);
	}

	printf("%s\n", TEST_PASS);
	tests_passed++;
}

/*
 * Test 3: NULL payload handling
 */
static void
test_null_payload(void)
{
	printf("Test 3: NULL payload handling... ");

	const int num_units = 50;
	struct drr_work_unit *units[num_units];

	/*
	 * Submit mix of NULL and non-NULL payloads
	 */
	for (int i = 0; i < num_units; i++) {
		units[i] = malloc(sizeof (struct drr_work_unit));
		memset(units[i], 0, sizeof (*units[i]));
		units[i]->sequence_num = i;

		if (i % 3 == 0) {
			/*
			 * NULL payload
			 */
			units[i]->payload = NULL;
			units[i]->payload_size = 0;
		} else {
			units[i]->payload_size = 1024 + (rand() % 8192);
			units[i]->payload = generate_random_payload(
			    units[i]->payload_size);
		}

		blake3_team_submit(units[i]);
	}

	/*
	 * Retrieve and verify ordering maintained
	 */
	for (int i = 0; i < num_units; i++) {
		struct drr_work_unit *result = blake3_team_retrieve();

		if (result->sequence_num != (uint64_t)i) {
			printf("%s - Out of order with NULL payloads\n",
			    TEST_FAIL);
			tests_failed++;
			return;
		}
	}

	/*
	 * Cleanup
	 */
	for (int i = 0; i < num_units; i++) {
		if (units[i]->payload != NULL)
			free(units[i]->payload);
		free(units[i]);
	}

	printf("%s\n", TEST_PASS);
	tests_passed++;
}

/*
 * Test 4: Large batch to test queue wrapping and threading
 */
static void
test_large_batch(void)
{
	printf("Test 4: Large batch (queue wrapping)... ");

	const int num_units = 500;
	struct drr_work_unit *units[num_units];
	zio_checksum_t expected_hashes[num_units];

	/*
	 * Create work units
	 */
	for (int i = 0; i < num_units; i++) {
		units[i] = malloc(sizeof (struct drr_work_unit));
		memset(units[i], 0, sizeof (*units[i]));

		units[i]->payload_size = 512 + (rand() % (512 * 1024));
		units[i]->payload = generate_random_payload(
		    units[i]->payload_size);
		units[i]->sequence_num = i;

		compute_blake3(units[i]->payload, units[i]->payload_size,
		    &expected_hashes[i]);
	}

	/*
	 * Submit all units
	 */
	for (int i = 0; i < num_units; i++) {
		blake3_team_submit(units[i]);
	}

	/*
	 * Retrieve and verify
	 */
	for (int i = 0; i < num_units; i++) {
		struct drr_work_unit *result = blake3_team_retrieve();

		if (result->sequence_num != (uint64_t)i) {
			printf("%s - Out of order in large batch\n",
			    TEST_FAIL);
			tests_failed++;
			return;
		}

		if (!blake3_equal(&result->blake3, &expected_hashes[i])) {
			printf("%s - Hash mismatch in large batch\n",
			    TEST_FAIL);
			tests_failed++;
			return;
		}
	}

	/*
	 * Cleanup
	 */
	for (int i = 0; i < num_units; i++) {
		free(units[i]->payload);
		free(units[i]);
	}

	printf("%s\n", TEST_PASS);
	tests_passed++;
}

/*
 * Test 5: Various payload sizes from 512 bytes to 1MB
 */
static void
test_various_sizes(void)
{
	printf("Test 5: Various payload sizes... ");

	uint64_t sizes[] = {
		512,
		1024,
		4096,
		8192,
		16384,
		32768,
		65536,
		131072,
		262144,
		524288,
		1048576
	};
	int num_sizes = sizeof (sizes) / sizeof (sizes[0]);

	for (int i = 0; i < num_sizes; i++) {
		struct drr_work_unit *unit = malloc(
		    sizeof (struct drr_work_unit));
		memset(unit, 0, sizeof (*unit));

		unit->payload_size = sizes[i];
		unit->payload = generate_random_payload(unit->payload_size);
		unit->sequence_num = i;

		zio_checksum_t expected_hash;
		compute_blake3(unit->payload, unit->payload_size,
		    &expected_hash);

		blake3_team_submit(unit);
		struct drr_work_unit *result = blake3_team_retrieve();

		if (!blake3_equal(&result->blake3, &expected_hash)) {
			printf("%s - Hash mismatch for size %lu\n",
			    TEST_FAIL, sizes[i]);
			tests_failed++;
			free(unit->payload);
			free(unit);
			return;
		}

		free(unit->payload);
		free(unit);
	}

	printf("%s\n", TEST_PASS);
	tests_passed++;
}

/*
 * Test 6: Interleaved submit and retrieve
 */
static void
test_interleaved(void)
{
	printf("Test 6: Interleaved submit/retrieve... ");

	const int num_units = 200;
	struct drr_work_unit *units[num_units];
	zio_checksum_t expected_hashes[num_units];
	int submit_idx = 0;
	int retrieve_idx = 0;

	/*
	 * Create all units first
	 */
	for (int i = 0; i < num_units; i++) {
		units[i] = malloc(sizeof (struct drr_work_unit));
		memset(units[i], 0, sizeof (*units[i]));

		units[i]->payload_size = 1024 + (rand() % (128 * 1024));
		units[i]->payload = generate_random_payload(
		    units[i]->payload_size);
		units[i]->sequence_num = i;

		compute_blake3(units[i]->payload, units[i]->payload_size,
		    &expected_hashes[i]);
	}

	/*
	 * Interleave submits and retrieves
	 */
	while (retrieve_idx < num_units) {
		/*
		 * Submit a few
		 */
		for (int i = 0; i < 10 && submit_idx < num_units; i++) {
			blake3_team_submit(units[submit_idx]);
			submit_idx++;
		}

		/*
		 * Retrieve a few
		 */
		for (int i = 0; i < 5 && retrieve_idx < num_units; i++) {
			struct drr_work_unit *result =
			    blake3_team_retrieve();

			if (result->sequence_num != (uint64_t)retrieve_idx) {
				printf("%s - Out of order in interleaved\n",
				    TEST_FAIL);
				tests_failed++;
				return;
			}

			if (!blake3_equal(&result->blake3,
			    &expected_hashes[retrieve_idx])) {
				printf("%s - Hash mismatch in interleaved\n",
				    TEST_FAIL);
				tests_failed++;
				return;
			}

			retrieve_idx++;
		}
	}

	/*
	 * Cleanup
	 */
	for (int i = 0; i < num_units; i++) {
		free(units[i]->payload);
		free(units[i]);
	}

	printf("%s\n", TEST_PASS);
	tests_passed++;
}

/*
 * Test 7: Stress test with very large payloads
 */
static void
test_stress(void)
{
	printf("Test 7: Stress test (large payloads)... ");

	const int num_units = 50;
	struct drr_work_unit *units[num_units];
	zio_checksum_t expected_hashes[num_units];

	/*
	 * Use large payloads to stress the system
	 */
	for (int i = 0; i < num_units; i++) {
		units[i] = malloc(sizeof (struct drr_work_unit));
		memset(units[i], 0, sizeof (*units[i]));

		/*
		 * 512KB to 1MB payloads
		 */
		units[i]->payload_size = (512 * 1024) +
		    (rand() % (512 * 1024));
		units[i]->payload = generate_random_payload(
		    units[i]->payload_size);
		units[i]->sequence_num = i;

		compute_blake3(units[i]->payload, units[i]->payload_size,
		    &expected_hashes[i]);

		blake3_team_submit(units[i]);
	}

	/*
	 * Retrieve all
	 */
	for (int i = 0; i < num_units; i++) {
		struct drr_work_unit *result = blake3_team_retrieve();

		if (result->sequence_num != (uint64_t)i) {
			printf("%s - Out of order in stress test\n",
			    TEST_FAIL);
			tests_failed++;
			return;
		}

		if (!blake3_equal(&result->blake3, &expected_hashes[i])) {
			printf("%s - Hash mismatch in stress test\n",
			    TEST_FAIL);
			tests_failed++;
			return;
		}
	}

	/*
	 * Cleanup
	 */
	for (int i = 0; i < num_units; i++) {
		free(units[i]->payload);
		free(units[i]);
	}

	printf("%s\n", TEST_PASS);
	tests_passed++;
}

int
main(void)
{
	printf("Blake3 Team Hash Tests\n");
	printf("======================\n\n");

	/*
	 * Seed random number generator
	 */
	srand(time(NULL));

	/*
	 * Initialize the blake3 team
	 */
	blake3_team_init();

	/*
	 * Run all tests
	 */
	test_single_unit();
	test_ordering();
	test_null_payload();
	test_large_batch();
	test_various_sizes();
	test_interleaved();
	test_stress();

	/*
	 * Print summary
	 */
	printf("\n======================\n");
	printf("Tests passed: %lu\n", tests_passed);
	printf("Tests failed: %lu\n", tests_failed);

	if (tests_failed > 0) {
		printf("\n\033[31mSOME TESTS FAILED\033[0m\n");
		return (1);
	} else {
		printf("\n\033[32mALL TESTS PASSED\033[0m\n");
		return (0);
	}
}
