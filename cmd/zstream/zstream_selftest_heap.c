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
 * Selftests for heap_t, the binary min/max heap of uint64_t values.
 *
 * Most tests run the heap side by side with a reference model: an unsorted
 * array that finds the next value to pop by linear scan. Every pop must
 * return exactly what the model says it should, and the heap's internal
 * state is audited as the test goes.
 *
 * The audit uses details that zstream_heap.h exposes but doesn't document:
 * heap_t's fields, and the standard implicit layout in which the children
 * of slot i are slots 2i + 1 and 2i + 2. If the implementation changes its
 * layout, audit_heap() must change with it.
 *
 * Fatal conditions (an overfull heap, invalid arguments) are tested in
 * forked children with selftest_expect_exit(). Several of these match the
 * text of a VERIFY condition in zstream_heap.c, so rewording one of those
 * assertions may require updating the expected message here.
 */

#include <assert.h>
#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>

#include "zstream_heap.h"
#include "zstream_selftest.h"
#include "zstream_util.h"

/* Planted in heap_pop()'s output argument to detect stray writes */
#define	SENTINEL		0x5a5a5a5aa5a5a5a5ULL

#define	HIGH_BIT		(1ULL << 63)

/*
 * Auditing the whole heap after every operation is quadratic, so large
 * heaps are audited only every AUDIT_PERIOD operations.
 */
#define	AUDIT_ALWAYS_BELOW	128
#define	AUDIT_PERIOD		64

#define	CTX_LEN			160

static const heap_order_t orders[] = { HEAP_MIN, HEAP_MAX };

/*
 * Values chosen to catch signed comparisons, truncation to 32 bits, and
 * off-by-one errors at both ends of the range
 */
static const uint64_t extreme_values[] = {
	0, 1, 2,
	0x7fffffffULL, 0x80000000ULL, 0xffffffffULL,
	0x100000000ULL, 0x100000001ULL,
	HIGH_BIT - 1, HIGH_BIT, HIGH_BIT + 1,
	UINT64_MAX - 1, UINT64_MAX,
};

static const char *
order_name(heap_order_t order)
{
	return (order == HEAP_MIN ? "min" : "max");
}

/*
 * Must a precede b in a heap of this order? Equal values precede neither
 * way.
 */
static boolean_t
precedes(heap_order_t order, uint64_t a, uint64_t b)
{
	return (order == HEAP_MIN ? a < b : a > b);
}

/*
 * The documented result of heap_compare_root(): a numeric comparison of
 * the probe with the root that doesn't depend on the heap's order.
 */
static int
expected_compare(uint64_t root, uint64_t value)
{
	if (root == value)
		return (0);
	return (root < value ? -1 : 1);
}

/*
 * Check the heap's bookkeeping and the heap property: no value precedes
 * its parent.
 */
static void
audit_heap(const heap_t *heap, heap_order_t order, uint64_t count,
    const char *ctx)
{
	if (heap->order != order) {
		errx(1, "%s: heap's order is %d, expected %d", ctx,
		    (int)heap->order, (int)order);
	}
	if (heap_count(heap) != count) {
		errx(1, "%s: heap holds %ju values, expected %ju", ctx,
		    (uintmax_t)heap_count(heap), (uintmax_t)count);
	}
	if (heap->values == NULL)
		errx(1, "%s: heap has no value array", ctx);
	if (heap->size == 0 || heap->count > heap->size) {
		errx(1, "%s: heap's count %ju doesn't fit its size %ju", ctx,
		    (uintmax_t)heap->count, (uintmax_t)heap->size);
	}
	if (heap->max_size != 0 && heap->size > heap->max_size) {
		errx(1, "%s: heap's size %ju exceeds its limit %ju", ctx,
		    (uintmax_t)heap->size, (uintmax_t)heap->max_size);
	}
	for (uint64_t i = 1; i < count; i++) {
		uint64_t parent = (i - 1) / 2;
		if (precedes(order, heap->values[i], heap->values[parent])) {
			errx(1, "%s: %s-heap property violated: slot %ju "
			    "(0x%016jx) precedes its parent, slot %ju "
			    "(0x%016jx)", ctx, order_name(order),
			    (uintmax_t)i, (uintmax_t)heap->values[i],
			    (uintmax_t)parent, (uintmax_t)heap->values[parent]);
		}
	}
}

static void
check_compare(heap_t *heap, uint64_t root, uint64_t probe, const char *ctx)
{
	int expect = expected_compare(root, probe);
	int got = heap_compare_root(heap, probe);
	if (got != expect) {
		errx(1, "%s: heap_compare_root(0x%016jx) with root 0x%016jx "
		    "returned %d, expected %d", ctx, (uintmax_t)probe,
		    (uintmax_t)root, got, expect);
	}
}

/*
 * Pop and require the given value
 */
static void
pop_expect(heap_t *heap, uint64_t expect, const char *ctx)
{
	uint64_t value = ~expect;	/* Anything but the right answer */
	uint64_t count = heap_count(heap);

	if (!heap_pop(heap, &value)) {
		errx(1, "%s: pop failed with %ju values in the heap; "
		    "expected 0x%016jx", ctx, (uintmax_t)count,
		    (uintmax_t)expect);
	}
	if (value != expect) {
		errx(1, "%s: popped 0x%016jx, expected 0x%016jx", ctx,
		    (uintmax_t)value, (uintmax_t)expect);
	}
	if (heap_count(heap) != count - 1) {
		errx(1, "%s: pop left %ju values, expected %ju", ctx,
		    (uintmax_t)heap_count(heap), (uintmax_t)(count - 1));
	}
}

/*
 * Require the heap to be empty, by every means of observing it. Repeated
 * pops must neither succeed, nor write their output argument, nor
 * underflow the count.
 */
static void
check_empty(heap_t *heap, const char *ctx)
{
	static const uint64_t probes[] = { 0, 1, SENTINEL, UINT64_MAX };

	if (heap_count(heap) != 0) {
		errx(1, "%s: heap should be empty but holds %ju values", ctx,
		    (uintmax_t)heap_count(heap));
	}
	for (int i = 0; i < 3; i++) {
		uint64_t value = SENTINEL;
		if (heap_pop(heap, &value)) {
			errx(1, "%s: pop from an empty heap succeeded, "
			    "returning 0x%016jx", ctx, (uintmax_t)value);
		}
		if (value != SENTINEL) {
			errx(1, "%s: failed pop changed its output to "
			    "0x%016jx", ctx, (uintmax_t)value);
		}
		if (heap_count(heap) != 0) {
			errx(1, "%s: failed pop changed the count to %ju", ctx,
			    (uintmax_t)heap_count(heap));
		}
	}
	for (int i = 0; i < ARRAY_SIZE(probes); i++) {
		int got = heap_compare_root(heap, probes[i]);
		if (got != 0) {
			errx(1, "%s: heap_compare_root(0x%016jx) on an empty "
			    "heap returned %d, expected 0", ctx,
			    (uintmax_t)probes[i], got);
		}
	}
}

/*
 * The reference model: an unsorted array of everything in the heap
 */
typedef struct {
	heap_order_t	m_order;
	uint64_t	m_count;
	uint64_t	m_cap;
	uint64_t	m_ops;
	uint64_t	*m_values;
} model_t;

static void
model_init(model_t *m, heap_order_t order, uint64_t cap)
{
	m->m_order = order;
	m->m_count = 0;
	m->m_cap = cap;
	m->m_ops = 0;
	m->m_values = safe_malloc(MAX(cap, 1) * sizeof (uint64_t));
}

static void
model_fini(model_t *m)
{
	free(m->m_values);
	m->m_values = NULL;
}

static uint64_t
model_root_index(const model_t *m)
{
	VERIFY3U(m->m_count, >, 0);
	uint64_t best = 0;
	for (uint64_t i = 1; i < m->m_count; i++) {
		if (precedes(m->m_order, m->m_values[i], m->m_values[best]))
			best = i;
	}
	return (best);
}

static uint64_t
model_root(const model_t *m)
{
	return (m->m_values[model_root_index(m)]);
}

static boolean_t
model_full(const model_t *m)
{
	return (m->m_count >= m->m_cap);
}

static void
maybe_audit(heap_t *heap, model_t *m, const char *ctx)
{
	m->m_ops++;
	if (m->m_count < AUDIT_ALWAYS_BELOW || m->m_ops % AUDIT_PERIOD == 0)
		audit_heap(heap, m->m_order, m->m_count, ctx);
}

/*
 * Push onto both the heap and the model
 */
static void
both_push(heap_t *heap, model_t *m, uint64_t value, const char *ctx)
{
	VERIFY3U(m->m_count, <, m->m_cap);
	heap_push(heap, value);
	m->m_values[m->m_count++] = value;
	maybe_audit(heap, m, ctx);
}

/*
 * Pop from both the heap and the model and require the same answer. If
 * the model is empty, require the heap to be empty as well.
 */
static void
both_pop(heap_t *heap, model_t *m, const char *ctx)
{
	if (m->m_count == 0) {
		check_empty(heap, ctx);
		return;
	}
	uint64_t ix = model_root_index(m);
	uint64_t expect = m->m_values[ix];
	m->m_values[ix] = m->m_values[--m->m_count];
	pop_expect(heap, expect, ctx);
	maybe_audit(heap, m, ctx);
}

static void
both_drain(heap_t *heap, model_t *m, const char *ctx)
{
	while (m->m_count > 0)
		both_pop(heap, m, ctx);
	audit_heap(heap, m->m_order, 0, ctx);
	check_empty(heap, ctx);
}

/*
 * Push values[0..n) onto a fresh heap that starts with a single slot,
 * checking the root after each push, and then pop everything.
 */
static void
push_and_drain(heap_order_t order, const uint64_t *values, uint64_t n,
    const char *ctx)
{
	heap_t heap = heap_init(order, 1, 0);
	model_t m;

	model_init(&m, order, n);
	for (uint64_t i = 0; i < n; i++) {
		both_push(&heap, &m, values[i], ctx);
		check_compare(&heap, model_root(&m), model_root(&m), ctx);
	}
	both_drain(&heap, &m, ctx);
	model_fini(&m);
	heap_fini(&heap);
}

/*
 * Value generators for randomized tests
 */
#define	NUM_DISTS	4

static const char *dist_names[NUM_DISTS] = {
	"uniform", "small range", "near high bit", "near extremes"
};

static uint64_t
random_value(selftest_rng_t *rng, int dist)
{
	switch (dist) {
	case 0:
		return (selftest_rng_next(rng));
	case 1:
		/* Lots of duplicates and ties between siblings */
		return (selftest_rng_below(rng, 8));
	case 2:
		/* Straddles the sign bit */
		return (HIGH_BIT - 16 + selftest_rng_below(rng, 32));
	default:
		if (selftest_rng_below(rng, 8) == 0)
			return (selftest_rng_below(rng, 4));
		return (UINT64_MAX - selftest_rng_below(rng, 16));
	}
}

static void
shuffle(selftest_rng_t *rng, uint64_t *values, uint64_t n)
{
	for (uint64_t i = n; i > 1; i--) {
		uint64_t j = selftest_rng_below(rng, i);
		uint64_t tmp = values[i - 1];
		values[i - 1] = values[j];
		values[j] = tmp;
	}
}

/*
 * A new heap is empty and records its parameters, and an emptied heap
 * behaves just like a new one.
 */
static void
heap_empty(void)
{
	static const struct {
		uint64_t	initial;
		uint64_t	max;
	} shapes[] = {
		{ 1, 0 }, { 1, 1 }, { 4, 4 }, { 16, 0 }, { 3, 1000 },
	};
	char ctx[CTX_LEN];

	for (int o = 0; o < ARRAY_SIZE(orders); o++) {
		for (int s = 0; s < ARRAY_SIZE(shapes); s++) {
			heap_order_t order = orders[o];
			uint64_t initial = shapes[s].initial;
			uint64_t max = shapes[s].max;
			(void) snprintf(ctx, sizeof (ctx),
			    "%s-heap, initial %ju, max %ju", order_name(order),
			    (uintmax_t)initial, (uintmax_t)max);

			heap_t heap = heap_init(order, initial, max);
			if (heap.order != order || heap.size != initial ||
			    heap.max_size != max || heap.count != 0 ||
			    heap.values == NULL) {
				errx(1, "%s: heap_init() returned order %d, "
				    "size %ju, max_size %ju, count %ju, "
				    "values %p", ctx, (int)heap.order,
				    (uintmax_t)heap.size,
				    (uintmax_t)heap.max_size,
				    (uintmax_t)heap.count, (void *)heap.values);
			}
			audit_heap(&heap, order, 0, ctx);
			check_empty(&heap, ctx);

			for (int round = 0; round < 3; round++) {
				heap_push(&heap, 42);
				audit_heap(&heap, order, 1, ctx);
				check_compare(&heap, 42, 42, ctx);
				pop_expect(&heap, 42, ctx);
				audit_heap(&heap, order, 0, ctx);
				check_empty(&heap, ctx);
			}
			if (heap.size != initial) {
				errx(1, "%s: size changed from %ju to %ju "
				    "without exceeding it", ctx,
				    (uintmax_t)initial, (uintmax_t)heap.size);
			}
			heap_fini(&heap);
		}
	}
}

/*
 * A one-element heap at its size limit, refilled repeatedly with each of
 * the extreme values
 */
static void
heap_single(void)
{
	char ctx[CTX_LEN];

	for (int o = 0; o < ARRAY_SIZE(orders); o++) {
		heap_order_t order = orders[o];
		heap_t heap = heap_init(order, 1, 1);

		for (int i = 0; i < ARRAY_SIZE(extreme_values); i++) {
			uint64_t v = extreme_values[i];
			(void) snprintf(ctx, sizeof (ctx),
			    "%s-heap holding only 0x%016jx", order_name(order),
			    (uintmax_t)v);
			heap_push(&heap, v);
			audit_heap(&heap, order, 1, ctx);
			for (int j = 0; j < ARRAY_SIZE(extreme_values); j++)
				check_compare(&heap, v, extreme_values[j], ctx);
			pop_expect(&heap, v, ctx);
			check_empty(&heap, ctx);
			VERIFY3U(heap.size, ==, 1);
		}
		heap_fini(&heap);
	}
}

/*
 * heap_compare_root() compares numerically, whatever the heap's order,
 * looks only at the root, and doesn't change the heap.
 */
static void
heap_compare_root_test(void)
{
	char ctx[CTX_LEN];

	for (int o = 0; o < ARRAY_SIZE(orders); o++) {
		heap_order_t order = orders[o];

		/* Every extreme value as the root against every other */
		for (int r = 0; r < ARRAY_SIZE(extreme_values); r++) {
			uint64_t root = extreme_values[r];
			(void) snprintf(ctx, sizeof (ctx),
			    "%s-heap with root 0x%016jx", order_name(order),
			    (uintmax_t)root);

			/*
			 * Add values that belong below the root, so the
			 * probes can't be answered correctly by looking
			 * anywhere else.
			 */
			heap_t heap = heap_init(order, 1, 0);
			uint64_t below = (order == HEAP_MIN) ?
			    UINT64_MAX : 0;
			heap_push(&heap, below);
			heap_push(&heap, root);
			heap_push(&heap, below);
			heap_push(&heap, root);
			audit_heap(&heap, order, 4, ctx);

			for (int p = 0; p < ARRAY_SIZE(extreme_values); p++) {
				check_compare(&heap, root, extreme_values[p],
				    ctx);
			}
			pop_expect(&heap, root, ctx);
			pop_expect(&heap, root, ctx);
			pop_expect(&heap, below, ctx);
			pop_expect(&heap, below, ctx);
			check_empty(&heap, ctx);
			heap_fini(&heap);
		}

		/* A peek must not disturb anything */
		(void) snprintf(ctx, sizeof (ctx), "%s-heap, peek side effects",
		    order_name(order));
		heap_t heap = heap_init(order, 4, 0);
		static const uint64_t contents[] = { 5, 10, 20, 7, 7, 13, 1 };
		for (int i = 0; i < ARRAY_SIZE(contents); i++)
			heap_push(&heap, contents[i]);
		uint64_t root = (order == HEAP_MIN) ? 1 : 20;
		heap_t before = heap;
		uint64_t snapshot[ARRAY_SIZE(contents)];
		memcpy(snapshot, heap.values, sizeof (snapshot));

		for (uint64_t probe = 0; probe <= 25; probe++)
			check_compare(&heap, root, probe, ctx);
		if (heap.order != before.order || heap.size != before.size ||
		    heap.max_size != before.max_size ||
		    heap.count != before.count ||
		    heap.values != before.values ||
		    memcmp(snapshot, heap.values, sizeof (snapshot)) != 0) {
			errx(1, "%s: heap_compare_root() modified the heap",
			    ctx);
		}
		heap_fini(&heap);
	}
}

/*
 * Every extreme value, twice, in many random orders
 */
static void
heap_extreme_values(void)
{
	const uint64_t n = 2 * ARRAY_SIZE(extreme_values);
	uint64_t values[2 * ARRAY_SIZE(extreme_values)];
	char ctx[CTX_LEN];
	selftest_rng_t rng;

	selftest_rng_init(&rng, 100);
	for (uint64_t i = 0; i < n; i++)
		values[i] = extreme_values[i % ARRAY_SIZE(extreme_values)];

	for (int round = 0; round < 64; round++) {
		shuffle(&rng, values, n);
		for (int o = 0; o < ARRAY_SIZE(orders); o++) {
			(void) snprintf(ctx, sizeof (ctx),
			    "%s-heap, extreme values round %d",
			    order_name(orders[o]), round);
			push_and_drain(orders[o], values, n, ctx);
		}
	}
}

/*
 * Structured input sequences of every length up to 100. Sorted and
 * reverse-sorted input are the best and worst cases for sifting up, and
 * the even and odd lengths alternate between a last parent with one child
 * and one with two.
 */
#define	NUM_PATTERNS	8

static const char *pattern_names[NUM_PATTERNS] = {
	"ascending", "descending", "constant", "zigzag", "organ pipe",
	"sawtooth", "across high bit", "pseudo-random bits"
};

static uint64_t
pattern_value(int pattern, uint64_t i, uint64_t n)
{
	switch (pattern) {
	case 0:
		return (i);
	case 1:
		return (n - i);
	case 2:
		return (7);
	case 3:
		return ((i & 1) ? UINT64_MAX - i : i);
	case 4:
		return (MIN(i, n - 1 - i));
	case 5:
		return (i % 5);
	case 6:
		return (HIGH_BIT - n / 2 + i);
	default:
		return (selftest_mix64(selftest_seed + i) & 1);
	}
}

static void
heap_patterns(void)
{
	uint64_t values[100];
	char ctx[CTX_LEN];

	for (int p = 0; p < NUM_PATTERNS; p++) {
		for (uint64_t n = 0; n <= ARRAY_SIZE(values); n++) {
			for (uint64_t i = 0; i < n; i++)
				values[i] = pattern_value(p, i, n);
			for (int o = 0; o < ARRAY_SIZE(orders); o++) {
				(void) snprintf(ctx, sizeof (ctx),
				    "%s-heap, %s pattern of %ju values",
				    order_name(orders[o]), pattern_names[p],
				    (uintmax_t)n);
				push_and_drain(orders[o], values, n, ctx);
			}
		}
	}
}

/*
 * Rearrange values into the next lexicographic permutation. Returns
 * B_FALSE, leaving the values sorted again, after the last one.
 */
static boolean_t
next_permutation(uint64_t *values, uint64_t n)
{
	if (n < 2)
		return (B_FALSE);
	uint64_t i = n - 1;
	while (i > 0 && values[i - 1] >= values[i])
		i--;
	if (i > 0) {
		uint64_t j = n - 1;
		while (values[j] <= values[i - 1])
			j--;
		uint64_t tmp = values[i - 1];
		values[i - 1] = values[j];
		values[j] = tmp;
	}
	for (uint64_t lo = i, hi = n - 1; lo < hi; lo++, hi--) {
		uint64_t tmp = values[lo];
		values[lo] = values[hi];
		values[hi] = tmp;
	}
	return (i > 0);
}

/*
 * Describe the sequence of small values (or of the top bits of large ones)
 * as a list of digits for error messages
 */
static void
describe_sequence(char *buf, size_t len, const char *prefix,
    const uint64_t *values, const uint64_t *alphabet, uint64_t n)
{
	size_t used = snprintf(buf, len, "%s", prefix);
	for (uint64_t i = 0; i < n && used < len; i++) {
		uint64_t digit = 0;
		while (alphabet[digit] != values[i])
			digit++;
		used += snprintf(buf + used, len - used, "%s%ju",
		    i == 0 ? "" : ",", (uintmax_t)digit);
	}
}

/*
 * Every permutation of up to 8 distinct values. The values spread across
 * the whole 64-bit range, so signed or truncated comparisons fail too.
 */
#define	MAX_PERMUTED	8

static void
heap_permutations(void)
{
	uint64_t alphabet[MAX_PERMUTED];
	uint64_t values[MAX_PERMUTED];
	char prefix[CTX_LEN];
	char ctx[CTX_LEN];

	for (uint64_t i = 0; i < MAX_PERMUTED; i++)
		alphabet[i] = (i << 61) | i;

	for (uint64_t n = 0; n <= MAX_PERMUTED; n++) {
		memcpy(values, alphabet, sizeof (values));
		uint64_t perms = 0;
		do {
			for (int o = 0; o < ARRAY_SIZE(orders); o++) {
				(void) snprintf(prefix, sizeof (prefix),
				    "%s-heap, permutation ",
				    order_name(orders[o]));
				describe_sequence(ctx, sizeof (ctx), prefix,
				    values, alphabet, n);
				push_and_drain(orders[o], values, n, ctx);
			}
			perms++;
		} while (next_permutation(values, n));

		uint64_t factorial = 1;
		for (uint64_t k = 2; k <= n; k++)
			factorial *= k;
		VERIFY3U(perms, ==, factorial);
	}
}

/*
 * Every sequence, up to a length limit, drawn from small alphabets of
 * values. These have many duplicates, so sifts constantly compare equal
 * values, and include values on either side of the sign bit.
 */
static void
heap_multisets(void)
{
	static const uint64_t alphabet[] = { 0, HIGH_BIT, UINT64_MAX, 1 };
	static const struct {
		uint64_t	symbols;
		uint64_t	max_len;
	} shapes[] = {
		{ 1, 16 }, { 2, 12 }, { 3, 9 }, { 4, 7 },
	};
	uint64_t values[16];
	char prefix[CTX_LEN];
	char ctx[CTX_LEN];

	for (int s = 0; s < ARRAY_SIZE(shapes); s++) {
		uint64_t symbols = shapes[s].symbols;
		for (uint64_t len = 0; len <= shapes[s].max_len; len++) {
			uint64_t total = 1;
			for (uint64_t i = 0; i < len; i++)
				total *= symbols;
			for (uint64_t code = 0; code < total; code++) {
				uint64_t c = code;
				for (uint64_t i = 0; i < len; i++) {
					values[i] = alphabet[c % symbols];
					c /= symbols;
				}
				for (int o = 0; o < ARRAY_SIZE(orders); o++) {
					(void) snprintf(prefix, sizeof (prefix),
					    "%s-heap, sequence ",
					    order_name(orders[o]));
					describe_sequence(ctx, sizeof (ctx),
					    prefix, values, alphabet, len);
					push_and_drain(orders[o], values, len,
					    ctx);
				}
			}
		}
	}
}

/*
 * Without a limit, the array starts at the initial size and doubles each
 * time it fills. It never shrinks.
 */
static void
heap_growth(void)
{
	static const uint64_t initials[] = { 1, 2, 3, 5, 7, 64, 1000, 4096 };
	const uint64_t n = 5000;
	char ctx[CTX_LEN];
	selftest_rng_t rng;

	selftest_rng_init(&rng, 200);
	for (int o = 0; o < ARRAY_SIZE(orders); o++) {
		for (int s = 0; s < ARRAY_SIZE(initials); s++) {
			heap_order_t order = orders[o];
			uint64_t expect_size = initials[s];
			(void) snprintf(ctx, sizeof (ctx),
			    "%s-heap, initial size %ju", order_name(order),
			    (uintmax_t)expect_size);

			heap_t heap = heap_init(order, expect_size, 0);
			model_t m;
			model_init(&m, order, n);

			for (int fill = 0; fill < 2; fill++) {
				for (uint64_t i = 0; i < n; i++) {
					/* The second fill needs no growth */
					if (fill == 0 &&
					    heap.count == expect_size)
						expect_size *= 2;
					both_push(&heap, &m,
					    random_value(&rng, i % NUM_DISTS),
					    ctx);
					if (heap.size != expect_size) {
						errx(1, "%s: size is %ju after "
						    "%ju pushes, expected %ju",
						    ctx, (uintmax_t)heap.size,
						    (uintmax_t)(i + 1),
						    (uintmax_t)expect_size);
					}
				}
				audit_heap(&heap, order, n, ctx);
				while (m.m_count > 0) {
					both_pop(&heap, &m, ctx);
					if (heap.size != expect_size) {
						errx(1, "%s: size changed from "
						    "%ju to %ju during pops",
						    ctx,
						    (uintmax_t)expect_size,
						    (uintmax_t)heap.size);
					}
				}
				check_empty(&heap, ctx);
			}
			model_fini(&m);
			heap_fini(&heap);
		}
	}
}

/*
 * With a limit, the array doubles until the next doubling would pass the
 * limit, and then grows to exactly the limit. A heap at its limit keeps
 * working as long as it isn't asked to hold more.
 */
static const struct {
	uint64_t	initial;
	uint64_t	max;
} limits[] = {
	{ 1, 1 }, { 1, 2 }, { 1, 3 }, { 1, 5 }, { 2, 2 }, { 3, 10 },
	{ 4, 16 }, { 5, 7 }, { 7, 7 }, { 6, 100 }, { 1, 1000 },
};

static void
heap_size_limit(void)
{
	char ctx[CTX_LEN];
	selftest_rng_t rng;

	selftest_rng_init(&rng, 300);
	for (int o = 0; o < ARRAY_SIZE(orders); o++) {
		for (int s = 0; s < ARRAY_SIZE(limits); s++) {
			heap_order_t order = orders[o];
			uint64_t max = limits[s].max;
			uint64_t expect_size = limits[s].initial;
			(void) snprintf(ctx, sizeof (ctx),
			    "%s-heap, initial size %ju, limit %ju",
			    order_name(order), (uintmax_t)expect_size,
			    (uintmax_t)max);

			heap_t heap = heap_init(order, expect_size, max);
			model_t m;
			model_init(&m, order, max);

			for (uint64_t i = 0; i < max; i++) {
				if (heap.count == expect_size)
					expect_size = MIN(2 * expect_size, max);
				both_push(&heap, &m,
				    random_value(&rng, i % NUM_DISTS), ctx);
				if (heap.size != expect_size) {
					errx(1, "%s: size is %ju after %ju "
					    "pushes, expected %ju", ctx,
					    (uintmax_t)heap.size,
					    (uintmax_t)(i + 1),
					    (uintmax_t)expect_size);
				}
			}
			if (heap.size != max) {
				errx(1, "%s: full heap's size is %ju", ctx,
				    (uintmax_t)heap.size);
			}

			for (uint64_t i = 0; i < 4 * max; i++) {
				both_pop(&heap, &m, ctx);
				both_push(&heap, &m,
				    random_value(&rng, i % NUM_DISTS), ctx);
				VERIFY3U(heap.size, ==, max);
			}
			both_drain(&heap, &m, ctx);
			VERIFY3U(heap.size, ==, max);
			model_fini(&m);
			heap_fini(&heap);
		}
	}
}

typedef struct {
	heap_order_t	la_order;
	uint64_t	la_initial;
	uint64_t	la_max;
} limit_arg_t;

static void
overfill_child(void *arg)
{
	limit_arg_t *la = arg;
	heap_t heap = heap_init(la->la_order, la->la_initial, la->la_max);
	uint64_t value;

	for (uint64_t i = 0; i < la->la_max; i++)
		heap_push(&heap, selftest_mix64(i));
	VERIFY(heap_pop(&heap, &value));
	heap_push(&heap, value);

	/* VERIFY failures here would not match the expected message */
	VERIFY3U(heap.count, ==, la->la_max);
	VERIFY3U(heap.size, ==, la->la_max);
	heap_push(&heap, value);

	/* Not reached if the limit is enforced */
	heap_fini(&heap);
}

/*
 * One push too many onto a heap at its limit is fatal.
 */
static void
heap_limit_exceeded(void)
{
	char what[CTX_LEN];
	char expect[CTX_LEN];

	for (int o = 0; o < ARRAY_SIZE(orders); o++) {
		for (int s = 0; s < ARRAY_SIZE(limits); s++) {
			limit_arg_t la = {
				.la_order = orders[o],
				.la_initial = limits[s].initial,
				.la_max = limits[s].max,
			};
			(void) snprintf(what, sizeof (what),
			    "overfill %s-heap, initial size %ju, limit %ju",
			    order_name(la.la_order), (uintmax_t)la.la_initial,
			    (uintmax_t)la.la_max);
			(void) snprintf(expect, sizeof (expect),
			    "heap size limit of %ju entries exceeded",
			    (uintmax_t)la.la_max);
			selftest_expect_exit(what, overfill_child, &la, expect);
		}
	}
}

typedef struct {
	const char	*ia_what;
	heap_order_t	ia_order;
	uint64_t	ia_initial;
	uint64_t	ia_max;
	const char	*ia_expect;
} init_arg_t;

static void
bad_init_child(void *arg)
{
	init_arg_t *ia = arg;
	heap_t heap = heap_init(ia->ia_order, ia->ia_initial, ia->ia_max);

	/* Not reached if the arguments are rejected */
	heap_fini(&heap);
}

/*
 * heap_init() rejects invalid arguments.
 */
static void
heap_bad_init(void)
{
	const uint64_t too_big = SIZE_MAX / sizeof (uint64_t) + 1ULL;
	init_arg_t cases[] = {
		{ "zero initial size", HEAP_MIN, 0, 0, "initial_size" },
		{ "zero initial size with limit", HEAP_MAX, 0, 10,
		    "initial_size" },
		{ "limit below initial size", HEAP_MIN, 10, 9, "max_size" },
		{ "limit of 1 below initial size", HEAP_MAX, 2, 1,
		    "max_size" },
		{ "invalid order", (heap_order_t)2, 1, 0, "HEAP_MIN" },
		{ "another invalid order", (heap_order_t)0x7f, 4, 8,
		    "HEAP_MIN" },
		{ "array too big for size_t", HEAP_MIN, too_big, 0,
		    "SIZE_MAX" },
		{ "array too big for size_t, with limit", HEAP_MAX, too_big,
		    too_big, "SIZE_MAX" },
		{ "maximal initial size", HEAP_MIN, UINT64_MAX, 0,
		    "SIZE_MAX" },
	};

	for (int i = 0; i < ARRAY_SIZE(cases); i++) {
		selftest_expect_exit(cases[i].ia_what, bad_init_child,
		    &cases[i], cases[i].ia_expect);
	}
}

/*
 * A heap so large that doubling its array would overflow a size_t. The
 * heap_t is forged, since a real one would need an exabyte of memory; the
 * push must fail its overflow check before it touches the array.
 */
static void
overflow_child(void *arg)
{
	uint64_t max_size = *(uint64_t *)arg;
	uint64_t limit = SIZE_MAX / sizeof (uint64_t);
	heap_t heap = {
		.order = HEAP_MIN,
		.size = limit,
		.max_size = max_size,
		.count = limit,
		.values = safe_malloc(sizeof (uint64_t)),
	};

	heap_push(&heap, 0);

	/* Not reached if the overflow is caught */
	heap_fini(&heap);
}

static void
heap_overflow_guard(void)
{
	uint64_t limit = SIZE_MAX / sizeof (uint64_t);
	uint64_t max_sizes[] = { 0, limit + 1, UINT64_MAX };
	char what[CTX_LEN];

	for (int i = 0; i < ARRAY_SIZE(max_sizes); i++) {
		(void) snprintf(what, sizeof (what),
		    "grow past SIZE_MAX with limit %ju",
		    (uintmax_t)max_sizes[i]);
		selftest_expect_exit(what, overflow_child, &max_sizes[i],
		    "SIZE_MAX");
	}
}

static void
fini_null_child(void *arg)
{
	(void) arg;
	heap_fini(NULL);
}

static void
fini_unallocated_child(void *arg)
{
	(void) arg;
	heap_t heap = { .order = HEAP_MIN };
	heap_fini(&heap);
}

/*
 * heap_fini() rejects a NULL heap or one with no array.
 */
static void
heap_bad_fini(void)
{
	selftest_expect_exit("heap_fini(NULL)", fini_null_child, NULL,
	    "heap != NULL");
	selftest_expect_exit("heap_fini() of a heap without an array",
	    fini_unallocated_child, NULL, "heap != NULL");
}

/*
 * Fill and drain over and over, sometimes completely and with extra pops
 * of the empty heap, and sometimes only partly. The array never shrinks.
 */
static void
heap_drain_refill(void)
{
	const uint64_t cap = 600;
	char ctx[CTX_LEN];
	selftest_rng_t rng;

	selftest_rng_init(&rng, 400);
	for (int o = 0; o < ARRAY_SIZE(orders); o++) {
		heap_order_t order = orders[o];
		heap_t heap = heap_init(order, 1, 0);
		model_t m;
		model_init(&m, order, cap);
		uint64_t last_size = heap.size;

		for (int cycle = 0; cycle < 200; cycle++) {
			(void) snprintf(ctx, sizeof (ctx), "%s-heap, cycle %d",
			    order_name(order), cycle);
			uint64_t target = selftest_rng_below(&rng, cap + 1);
			int dist = cycle % NUM_DISTS;
			while (m.m_count < target) {
				both_push(&heap, &m,
				    random_value(&rng, dist), ctx);
			}
			uint64_t pops = (cycle % 3 == 0) ?
			    m.m_count + 1 + selftest_rng_below(&rng, 3) :
			    selftest_rng_below(&rng, m.m_count + 1);
			for (uint64_t i = 0; i < pops; i++)
				both_pop(&heap, &m, ctx);
			audit_heap(&heap, order, m.m_count, ctx);

			if (heap.size < last_size) {
				errx(1, "%s: size shrank from %ju to %ju", ctx,
				    (uintmax_t)last_size,
				    (uintmax_t)heap.size);
			}
			last_size = heap.size;
		}
		both_drain(&heap, &m, ctx);
		model_fini(&m);
		heap_fini(&heap);
	}
}

/*
 * Several heaps of both orders in use at once mustn't share any state.
 */
#define	NUM_INDEPENDENT	6

static void
heap_independent(void)
{
	const uint64_t cap = 500;
	heap_t heaps[NUM_INDEPENDENT];
	model_t models[NUM_INDEPENDENT];
	char ctx[NUM_INDEPENDENT][CTX_LEN];
	selftest_rng_t rng;

	selftest_rng_init(&rng, 500);
	for (int i = 0; i < NUM_INDEPENDENT; i++) {
		heap_order_t order = orders[i % ARRAY_SIZE(orders)];
		heaps[i] = heap_init(order, 1 + i, (i & 2) ? cap : 0);
		model_init(&models[i], order, cap);
		(void) snprintf(ctx[i], sizeof (ctx[i]),
		    "heap %d of %d (%s)", i, NUM_INDEPENDENT,
		    order_name(order));
		for (int j = 0; j < i; j++)
			VERIFY3P(heaps[i].values, !=, heaps[j].values);
	}

	for (int op = 0; op < 50000; op++) {
		int i = selftest_rng_below(&rng, NUM_INDEPENDENT);
		if (!model_full(&models[i]) &&
		    selftest_rng_below(&rng, 100) < 55) {
			both_push(&heaps[i], &models[i],
			    random_value(&rng, op % NUM_DISTS), ctx[i]);
		} else {
			both_pop(&heaps[i], &models[i], ctx[i]);
		}
	}

	for (int i = 0; i < NUM_INDEPENDENT; i++) {
		both_drain(&heaps[i], &models[i], ctx[i]);
		model_fini(&models[i]);
		heap_fini(&heaps[i]);
	}
}

/*
 * Long random sequences of pushes, pops, and peeks checked against the
 * model. The push probability cycles through phases so that the heap
 * repeatedly grows to the model's capacity and shrinks back toward empty.
 */
static void
heap_random_model(void)
{
	static const int push_pct[] = { 75, 50, 25, 50 };
	const uint64_t cap = 2048;
	const int nops = 60000;
	const int phase_len = 2500;
	char ctx[CTX_LEN];
	selftest_rng_t rng;

	selftest_rng_init(&rng, 600);
	for (int o = 0; o < ARRAY_SIZE(orders); o++) {
		for (int dist = 0; dist < NUM_DISTS; dist++) {
			heap_order_t order = orders[o];
			uint64_t initial = 1 + selftest_rng_below(&rng, 16);
			uint64_t max = (dist & 1) ? cap : 0;
			(void) snprintf(ctx, sizeof (ctx),
			    "%s-heap, %s values, initial %ju, max %ju",
			    order_name(order), dist_names[dist],
			    (uintmax_t)initial, (uintmax_t)max);

			heap_t heap = heap_init(order, initial, max);
			model_t m;
			model_init(&m, order, cap);

			for (int op = 0; op < nops; op++) {
				int pct = push_pct[(op / phase_len) %
				    ARRAY_SIZE(push_pct)];
				if (!model_full(&m) &&
				    selftest_rng_below(&rng, 100) < pct) {
					both_push(&heap, &m,
					    random_value(&rng, dist), ctx);
				} else {
					both_pop(&heap, &m, ctx);
				}

				if (op % 4 != 0)
					continue;
				if (m.m_count == 0) {
					check_empty(&heap, ctx);
					continue;
				}
				uint64_t root = model_root(&m);
				check_compare(&heap, root, root, ctx);
				check_compare(&heap, root, root + 1, ctx);
				check_compare(&heap, root, root - 1, ctx);
				check_compare(&heap, root,
				    random_value(&rng, dist), ctx);
			}
			both_drain(&heap, &m, ctx);
			model_fini(&m);
			heap_fini(&heap);
		}
	}
}

/*
 * Heapsort a million values, some repeated, and check that they come out
 * in order and that the same multiset goes in as comes out. This test
 * checks the heap with order and fingerprints instead of the model.
 */
#define	LARGE_SORT_COUNT	(1ULL << 20)

typedef struct {
	uint64_t	fp_count;
	uint64_t	fp_sum;
	uint64_t	fp_mixsum;
	uint64_t	fp_mixxor;
} fingerprint_t;

static void
fingerprint_add(fingerprint_t *fp, uint64_t value)
{
	uint64_t mix = selftest_mix64(value);
	fp->fp_count++;
	fp->fp_sum += value;
	fp->fp_mixsum += mix;
	fp->fp_mixxor ^= selftest_mix64(mix);
}

static void
heap_large_sort(void)
{
	char ctx[CTX_LEN];
	selftest_rng_t rng;

	selftest_rng_init(&rng, 700);
	for (int o = 0; o < ARRAY_SIZE(orders); o++) {
		heap_order_t order = orders[o];
		heap_t heap = heap_init(order, 1, 0);
		fingerprint_t in = { 0 };
		fingerprint_t out = { 0 };
		uint64_t value = 0;
		(void) snprintf(ctx, sizeof (ctx), "%s-heap, large sort",
		    order_name(order));

		for (uint64_t i = 0; i < LARGE_SORT_COUNT; i++) {
			if (i == 0 || selftest_rng_below(&rng, 16) != 0)
				value = selftest_rng_next(&rng);
			heap_push(&heap, value);
			fingerprint_add(&in, value);
		}
		audit_heap(&heap, order, LARGE_SORT_COUNT, ctx);

		uint64_t prev = 0;
		for (uint64_t i = 0; i < LARGE_SORT_COUNT; i++) {
			if (!heap_pop(&heap, &value)) {
				errx(1, "%s: pop %ju of %ju failed", ctx,
				    (uintmax_t)i, (uintmax_t)LARGE_SORT_COUNT);
			}
			if (i > 0 && precedes(order, value, prev)) {
				errx(1, "%s: pop %ju returned 0x%016jx after "
				    "0x%016jx", ctx, (uintmax_t)i,
				    (uintmax_t)value, (uintmax_t)prev);
			}
			fingerprint_add(&out, value);
			prev = value;
		}
		check_empty(&heap, ctx);
		if (memcmp(&in, &out, sizeof (in)) != 0) {
			errx(1, "%s: the values popped are not the values "
			    "pushed", ctx);
		}
		heap_fini(&heap);
	}
}

const test_case_t selftest_heap_cases[] = {
	{ "heap_empty",			heap_empty },
	{ "heap_single",		heap_single },
	{ "heap_compare_root",		heap_compare_root_test },
	{ "heap_extreme_values",	heap_extreme_values },
	{ "heap_patterns",		heap_patterns },
	{ "heap_permutations",		heap_permutations },
	{ "heap_multisets",		heap_multisets },
	{ "heap_growth",		heap_growth },
	{ "heap_size_limit",		heap_size_limit },
	{ "heap_limit_exceeded",	heap_limit_exceeded },
	{ "heap_bad_init",		heap_bad_init },
	{ "heap_overflow_guard",	heap_overflow_guard },
	{ "heap_bad_fini",		heap_bad_fini },
	{ "heap_drain_refill",		heap_drain_refill },
	{ "heap_independent",		heap_independent },
	{ "heap_random_model",		heap_random_model },
	{ "heap_large_sort",		heap_large_sort },
	{ NULL,				NULL },
};
