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

/*
 * This is a watchdog timer and multithread backtrace dumper that's used by
 * zstream selftest. libspl_backtrace() does all the real work, but it can
 * only dump the current thread's stack. We need to get every thread to call
 * libspl_backtrace() in an organized sequence. However, there's no "give me
 * a list of all pthreads" function in the POSIX API.
 *
 * Rather than constructing an ad-hoc thread registry, we can approach the
 * problem by unblocking SIGRTMIN (an arbitrary choice) and designating a
 * handler for it when zstream first starts. All created threads then
 * inherit this signal mask and handler.
 *
 * The SIGRTMIN handler calls libspl_backtrace(), which is signal-handler
 * safe. It then signals a semaphore to indicate that it's finished and
 * enters pause().
 *
 * The watchdog itself is a separate thread trampolined through SIGALRM. It
 * blocks SIGRTMIN at the thread level and runs libspl_backtrace(). It then
 * enters a loop in which it sends SIGRTMIN to the process as a whole and
 * runs sem_timedwait() to see if any thread woke up and dumped its
 * backtrace. If that call times out, either the receiving thread wedged
 * while trying to backtrace or there were no more threads to backtrace.
 *
 * These two scenarios can be distinguished by calling sigpending(). If a
 * SIGRTMIN still shows as being pending on the process, then there was no
 * thread to receive it and we are done. If there's no pending signal, then
 * some thread did receive the signal but failed to post to the semaphore;
 * we print a "thread wedged while backtracing" message and continue the
 * loop.
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/backtrace.h>
#include <sys/stdtypes.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "zstream_backtrace.h"

/*
 * Watchdog timeout in seconds. A test that hits this limit is almost
 * certainly deadlocked, and the watchdog converts the hang into a test
 * failure instead of a stuck test run.
 */
#define	WATCHDOG_TIMEOUT_SECS	120
#define	MAX_SECS_FOR_BACKTRACE	2
#define	WATCHDOG_SIGNAL		SIGALRM
#define	THREAD_BT_SIGNAL	SIGRTMIN

static sem_t sem_thread_bt_complete;	/* From thread to watchdog */

static int watchdog_pipe[2];

static void
backtrace_self(int signum)
{
	if (write(STDERR_FILENO, "\n", 1) < 0)
		(void) signum;
	libspl_backtrace(STDERR_FILENO);
	sem_post(&sem_thread_bt_complete);
	pause();
}

static void
backtrace_all_threads(void)
{
	while (B_TRUE) {
		if (kill(getpid(), THREAD_BT_SIGNAL) != 0)
			err(1, "failed to send thread backtrace signal");
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += MAX_SECS_FOR_BACKTRACE;
		if (sem_timedwait(&sem_thread_bt_complete, &deadline) != 0) {
			sigset_t pending;
			if (sigpending(&pending) != 0)
				err(1, "sigpending failed");
			if (sigismember(&pending, THREAD_BT_SIGNAL)) {
				return;
			} else {
				warnx("a thread filed to generate a backtrace, "
				    "continuing...");
			}
		}
	}
}


/*
 * The watchdog's actual code is a bit too complex to fit in a signal handler.
 */
static void
watchdog_trampoline(int sig)
{
	(void) sig;
	int save = errno;
	char c = 'X';
	if (write(watchdog_pipe[1], &c, 1) != 1)
		err(1, "unable to write one byte to watchdog pipe");
	errno = save;
}

/*
 * Body of the watchdog thread. Any operation is OK here.
 */
static void *
watchdog(void *nope)
{
	(void) nope;

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, THREAD_BT_SIGNAL);
	if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0)
		err(1, "pthread_sigmask failed");

	for (;;) {
		char c;
		ssize_t n = read(watchdog_pipe[0], &c, 1);
		if (n == 1)
			break;
		if (n < 0 && errno == EINTR)
			continue;
		err(1, "bizarre read from watchdog pipe");
	}

	fprintf(stderr, "\n\nWATCHDOG TIMER EXPIRED\n"
	    "Dumping backtrace for all threads...\n\n");
	fflush(stderr);
	libspl_backtrace(STDERR_FILENO);
	backtrace_all_threads();
	fprintf(stderr, "\nAll threads backtraced, exiting.\n");
	exit(1);
}

/*
 * This function must be called before any (extra) threads are created.
 */
void
watchdog_init(void)
{
	if (pipe(watchdog_pipe) != 0)
		err(1, "failed to create watchdog timer pipe");
	for (int i = 0; i < 2; i++) {
		if (fcntl(watchdog_pipe[i], F_SETFD, FD_CLOEXEC) != 0)
			err(1, "fcntl on watchdog_pipe failed");
	}

	if (sem_init(&sem_thread_bt_complete, 0, 0) != 0)
		err(1, "watchdog sem_init failed");

	/*
	 * Unblock both WATCHDOG_SIGNAL and THREAD_BT_SIGNAL so that child
	 * threads inherit that mask without having to make any calls of
	 * their own.
	 */
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, WATCHDOG_SIGNAL);
	sigaddset(&mask, THREAD_BT_SIGNAL);
	if (pthread_sigmask(SIG_UNBLOCK, &mask, NULL) != 0)
		err(1, "pthread_sigmask failed");

	pthread_t tid;
	if (pthread_create(&tid, NULL, watchdog, NULL) != 0)
		err(1, "pthread_create for watchdog failed");
	if (pthread_setname_np(tid, "watchdog") != 0)
		err(1, "could not set name of watchdog thread");
	if (pthread_detach(tid) != 0)
		err(1, "failed to detach watchdog thread");

	struct sigaction sa = {
		.sa_handler = watchdog_trampoline,
		.sa_flags = SA_RESTART
	};
	if (sigaction(WATCHDOG_SIGNAL, &sa, NULL) != 0)
		err(1, "watchdog sigaction failed");
	sa.sa_handler = backtrace_self;
	if (sigaction(THREAD_BT_SIGNAL, &sa, NULL) != 0)
		err(1, "backtrace sigaction failed");
}

void
watchdog_arm(void)
{
	(void) alarm(WATCHDOG_TIMEOUT_SECS);
}

void
watchdog_disarm(void)
{
	(void) alarm(0);
}
