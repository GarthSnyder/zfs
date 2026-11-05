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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011, 2020 by Delphix. All rights reserved.
 * Copyright (c) 2012, Joyent, Inc. All rights reserved.
 * Copyright (c) 2012 Pawel Jakub Dawidek <pawel@dawidek.net>.
 * All rights reserved
 * Copyright (c) 2013 Steven Hartland. All rights reserved.
 * Copyright 2015, OmniTI Computer Consulting, Inc. All rights reserved.
 * Copyright 2016 Igor Kozhukhov <ikozhukhov@gmail.com>
 * Copyright (c) 2018, loli10K <ezomori.nozomu@gmail.com>. All rights reserved.
 * Copyright (c) 2019 Datto Inc.
 * Copyright (c) 2024, Klara, Inc.
 */

/*
 * The safe_ wrappers declared in zstream_util.h. They depend only on libc
 * and libspl, so unit tests can link them without libzfs or libzpool.
 */

#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "zstream_util.h"

/*
 * Hook for selftests to override pwrite()
 */
ssize_t (*safe_pwrite_call)(int, const void *, size_t, off_t) = pwrite;

void *
safe_malloc(size_t size)
{
	void *rv = malloc(size);
	if (rv == NULL) {
		errx(1, "failed to allocate %zu bytes, aborting...", size);
	}
	return (rv);
}

void *
safe_calloc(size_t size)
{
	void *rv = calloc(1, size);
	if (rv == NULL) {
		errx(1, "failed to allocate %zu bytes, aborting...", size);
	}
	return (rv);
}

void *
safe_realloc(void *buff, size_t new_size)
{
	if ((buff = realloc(buff, new_size)) == NULL && new_size != 0)
		errx(1, "failed to realloc %llu bytes, aborting...",
		    (u_longlong_t)new_size);
	return (buff);
}

void
safe_pwrite(int fd, const void *buf, size_t count, off_t offset,
    const char *where)
{
	size_t done = 0;

	while (done < count) {
		ssize_t n = safe_pwrite_call(fd, (const uint8_t *)buf + done,
		    count - done, offset + done);
		if (n < 0) {
			if (errno == EINTR)
				continue; /* zero progress this call, retry */
			else if ((errno == ENOSPC || errno == EDQUOT) &&
			    where != NULL)
				errx(1, "pwrite() failed, out of space in %s"
				    ", aborting...", where);
			err(1, "pwrite() failed");
		}
		if (n == 0)
			errx(1, "pwrite of %llu bytes failed",
			    (u_longlong_t)count);
		done += (size_t)n;
	}
}

void
safe_pread_zero(int fd, void *buf, size_t count, off_t offset)
{
	size_t done = 0;

	while (done < count) {
		ssize_t n = pread(fd, (uint8_t *)buf + done, count - done,
		    offset + done);
		if (n < 0) {
			if (errno == EINTR)
				continue; /* zero progress this call, retry */
			err(1, "pread failed");
		}
		if (n == 0) {
			memset((uint8_t *)buf + done, 0, count - done);
			return;
		}
		done += (size_t)n;
	}
}

void
safe_pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
	int error = pthread_sigmask(how, set, oldset);
	if (error != 0) {
		errno = error;
		err(1, "pthread_sigmask failed");
	}
}

pthread_t
safe_create_thread(thread_f *body, void *body_arg, const char *name,
    boolean_t detach)
{
	pthread_t tid;
	int ret;
	int name_attempts = 3;

	ret = pthread_create(&tid, NULL, body, body_arg);
	if (ret != 0) {
		errno = ret;
		err(1, "pthread_create for %s failed", name);
	}
	/*
	 * pthread_setname_np() fails randomly on some Debian systems
	 * because of difficulty reading /proc/self/task. The characteristic
	 * error is "No such file or directory." The name is just a
	 * debugging aid, so we can ignore the error, but we'll make three
	 * attempts to set it. This code previously printed a warning
	 * message, but that interferes with zstream dump output comparisons
	 * in ZTS.
	 */
	while (name_attempts-- > 0) {
		ret = pthread_setname_np(tid, name);
		if (ret == 0)
			break;
		usleep(100);
	}
	if (detach) {
		ret = pthread_detach(tid);
		if (ret != 0) {
			errno = ret;
			err(1, "failed to detach %s thread", name);
		}
	}
	return (tid);
}

int
safe_create_temp_file(const char *dir_path)
{
	char buff[1024];
	int length = snprintf(buff, sizeof (buff),
	    "%s/zstream-temp-XXXXXX", dir_path);
	if (length >= sizeof (buff))
		errx(1, "filename too long in %s", __func__);
	int fd = mkstemp(buff);
	if (fd < 0)
		err(1, "unable to create temp file in %s", dir_path);
	if (unlink(buff) < 0)
		err(1, "unlink of %s failed", buff);
	return (fd);
}
