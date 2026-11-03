/*
 * SPDX-License-Identifier: MIT
 *
 * f1_runner.c: the one daemon the F1 blob installs (/var/root/f1_test).
 *
 * Why a runner and not one daemon per test: launchd on this port loads its job
 * list from /System/Library/xpc/launchd.plist, a cache built with the rootfs --
 * a LaunchDaemon plist written at boot is installed and logged, but never
 * exec'd (ring-e2: "install complete (6 files)", "exec f1_test", no
 * com.sweet.e).  So the blob's one daemon runs the stages itself.
 *
 * Why E2 runs first: the F3 stage never returns from OSMesaDestroyContext().
 * ring-e2b and ring-e2c both end the stage on the GEM_CLOSE sequence of the
 * context teardown with nothing after it -- no return code, no next stage.  The
 * stage that has not proven itself goes first; whatever the F3 teardown does,
 * it cannot take E2 with it.
 *
 * Why each stage is forked: a stage that dies must not be able to skip the
 * rest, and the runner has to report how it ended.  A stage that stops making
 * progress is killed after STAGE_TIMEOUT_S and reported as such, because "the
 * child hung in teardown" and "the child crashed" are different answers.
 *
 * Why the child waits for the connection before the stage starts: a daemon
 * exec'd early in boot can reach SweetMali before IOServiceOpen is accepted,
 * and connect_once() does not cache a failure -- it forgets it and the next
 * call tries again.  A stage that starts in that window dies at its own
 * sweet_drm_open() and logs nothing at all, because sweet_drm_log() is a no-op
 * without a connection; that is exactly the silence ring-e2d shows for E2.  So
 * the child waits for the connection first and says so through it, and only
 * then calls the stage.  The runner itself never connects: a connection made
 * before fork() would be inherited by every child, and a user client belongs to
 * the task that opened it, so each child connects for itself.
 *
 * The /dev/console writes are a second, best-effort channel: os_log does not
 * reach the ring (ring-e2c has no markers while launchd's own console messages
 * are there), and if a child still ends up with no connection the console is
 * all it has left.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sweet_drm.h"

int f3_main(void);
int e_main(void);

#define STAGE_TIMEOUT_S		90
#define CONNECT_TIMEOUT_MS	120000

/* best effort: the kernel ring is reached through a connection, not here */
static void
console(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n, fd;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	printf("%s\n", buf);
	fflush(stdout);
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		(void)write(fd, buf, (size_t)n);
		(void)write(fd, "\n", 1);
		close(fd);
	}
}

static int
run_stage(const char *name, int (*fn)(void))
{
	pid_t pid;
	int st = 0, waited = 0;

	pid = fork();
	if (pid == 0) {
		int fd, rc, ms;

		fd = open("/dev/console", O_WRONLY | O_NOCTTY);
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2) {
				close(fd);
			}
		}

		/*
		 * The stand-in reads the trace flag when it first connects, and
		 * this child is about to connect: setting it here is what makes
		 * the stage's ioctl trace appear in the ring.  A stage that sets
		 * it in its own main() -- which is what F3's and E2's do -- sets
		 * it after that connection already exists, so the trace it asks
		 * for never turns on (ring-e2g has no "ioctl nr=..." lines from
		 * the E2 run at all).
		 */
		setenv("SWEET_DRM_TRACE", "1", 1);

		for (ms = 0; !drmAvailable(); ms += 100) {
			if (ms >= CONNECT_TIMEOUT_MS) {
				break;
			}
			usleep(100000);
		}
		if (!drmAvailable()) {
			console("F1RUN: stage %s: no SweetMali connection",
			    name);
			fflush(stdout);
			_exit(3);
		}
		sweet_drm_log("F1RUN: stage %s connected after %d ms", name,
		    ms);
		rc = fn();
		sweet_drm_log("F1RUN: stage %s returned %d", name, rc);
		fflush(stdout);
		_exit(rc & 0xff);
	}
	if (pid < 0) {
		console("F1RUN: stage %s fork failed: %d", name, errno);
		return -1;
	}

	for (;;) {
		pid_t r = waitpid(pid, &st, WNOHANG);

		if (r == pid) {
			break;
		}
		if (r < 0 && errno != EINTR) {
			console("F1RUN: stage %s waitpid failed: %d", name,
			    errno);
			return -1;
		}
		if (waited >= STAGE_TIMEOUT_S * 10) {
			console("F1RUN: stage %s still running after %ds",
			    name, STAGE_TIMEOUT_S);
			(void)kill(pid, SIGKILL);
			(void)waitpid(pid, &st, 0);
			return -2;
		}
		waited++;
		usleep(100000);
	}

	if (WIFEXITED(st)) {
		console("F1RUN: stage %s exit=%d", name, WEXITSTATUS(st));
		return WEXITSTATUS(st);
	}
	if (WIFSIGNALED(st)) {
		console("F1RUN: stage %s died, signal=%d", name, WTERMSIG(st));
		return -WTERMSIG(st);
	}
	console("F1RUN: stage %s ended, status=0x%x", name, st);
	return -1;
}

int
main(void)
{
	int e_rc, f3_rc;

	console("F1RUN: start, E2 then F3");
	e_rc = run_stage("E2 (IOSurface as BO)", e_main);
	f3_rc = run_stage("F3 (Mesa triangle)", f3_main);
	console("F1RUN: done, E2 rc=%d F3 rc=%d", e_rc, f3_rc);
	return (e_rc == 0 && f3_rc == 0) ? 0 : 1;
}