// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 * Copyright 2022 Axcient.  All rights reserved.
 * Copyright (c) 2022 by Delphix. All rights reserved.
 * Copyright (c) 2024, Klara, Inc.
 */

#include <stdio.h>
#include <stddef.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_ioctl.h>
#include <sys/dmu.h>

#ifndef	_ZSTREAM_SHARED_H
#define	_ZSTREAM_SHARED_H

#ifdef	__cplusplus
extern "C" {
#endif

extern void *safe_malloc(size_t size);
extern void *safe_calloc(size_t n);
extern int sfread(void *buf, size_t size, FILE *fp);
extern int dump_record(dmu_replay_record_t *drr, void *payload,
	int payload_len, zio_cksum_t *zc, int outfd);

#ifdef	__cplusplus
}
#endif

#endif	/* _ZSTREAM_SHARED_H */
