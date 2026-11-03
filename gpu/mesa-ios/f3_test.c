/*
 * SPDX-License-Identifier: MIT
 *
 * f3_test.c: the first triangle through Mesa on KL7.
 *
 *   OSMesa (ES profile 0x36) -> gallium panfrost -> libdrm stand-in (gpu/mali-sweet)
 *                            -> SweetMali user client -> Mali-G57
 *
 * No EAGL, no CoreAnimation: this only answers "does a GLES context over
 * panfrost compile a shader, render, and read the pixels back".  Every line
 * goes to stdout and, once the stand-in connection is up, to the kernel ring
 * (via sweet_drm_log), which is what survives a kernel that never reaches the
 * home screen.
 *
 * GL entry points are resolved through OSMesaGetProcAddress(), which is how
 * the EAGL shim will reach them too (libOSMesa does not export gl* directly).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "GL/osmesa.h"
#include "GLES2/gl2.h"

#include "sweet_drm.h"
#include "sweet_panfrost_ioctl.h"

#define W 256
#define H 256

#define LOG(...) do { \
	printf("F3: " __VA_ARGS__); \
	printf("\n"); \
	fflush(stdout); \
	sweet_drm_log("F3: " __VA_ARGS__); \
} while (0)

/*
 * iOS jetsam enforces a per-task physical-footprint limit, and a LaunchDaemon
 * inherits whatever launchd picked for it.  panfrost's tiler heap alone is a
 * 128 MiB BO that the driver maps into this process, so the footprint jumps by
 * that much during device init; the default limit killed the F3 test with
 * SIGKILL (OS_REASON_JETSAM / JETSAM_REASON_MEMORY_PERPROCESSLIMIT).  Raise our
 * own limit before talking to Mesa.  memorystatus_control(2) is syscall 440 and
 * checks only that the caller is root, so no entitlement is needed.
 */
#define SYS_MEMORYSTATUS_CONTROL 440
#define MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES 7
#define MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES 8
#define MEMORYSTATUS_MEMLIMIT_ATTR_FATAL 0x1

/* same layout as bsd/sys/kern_memorystatus.h; the kernel checks the size */
struct sweet_memlimit_props {
	int32_t active;
	uint32_t active_attr;
	int32_t inactive;
	uint32_t inactive_attr;
};

static long
sweet_syscall5(long n, long a0, long a1, long a2, long a3, long a4)
{
	/* Darwin arm64: the syscall number is x16, the trap is svc #0x80
	 * (xnu osfmk/arm64/proc_reg.h: ARM64_SYSCALL_CODE_REG_NUM 16). */
	register long x16 __asm__("x16") = n;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;

	__asm__ volatile("svc #0x80"
	    : "+r"(x0)
	    : "r"(x16), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
	    : "memory");
	return x0;
}

static void
raise_memlimit(void)
{
	struct sweet_memlimit_props p;
	long rc;

	memset(&p, 0, sizeof(p));
	rc = sweet_syscall5(SYS_MEMORYSTATUS_CONTROL,
	    MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, (long)getpid(), 0,
	    (long)(uintptr_t)&p, sizeof(p));
	LOG("memlimit before: rc=%ld active=%dMiB/0x%x inactive=%dMiB/0x%x",
	    rc, p.active, p.active_attr, p.inactive, p.inactive_attr);

	p.active = 2048;
	p.active_attr = MEMORYSTATUS_MEMLIMIT_ATTR_FATAL;
	p.inactive = 2048;
	p.inactive_attr = MEMORYSTATUS_MEMLIMIT_ATTR_FATAL;
	rc = sweet_syscall5(SYS_MEMORYSTATUS_CONTROL,
	    MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, (long)getpid(), 0,
	    (long)(uintptr_t)&p, sizeof(p));
	LOG("memlimit set 2048MiB: rc=%ld", rc);

	memset(&p, 0, sizeof(p));
	rc = sweet_syscall5(SYS_MEMORYSTATUS_CONTROL,
	    MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, (long)getpid(), 0,
	    (long)(uintptr_t)&p, sizeof(p));
	LOG("memlimit after: rc=%ld active=%dMiB/0x%x inactive=%dMiB/0x%x",
	    rc, p.active, p.active_attr, p.inactive, p.inactive_attr);
}

static void *
sym(const char *name)
{
	void *p = (void *)OSMesaGetProcAddress(name);

	if (p == NULL) {
		LOG("FAIL missing GL symbol %s", name);
		exit(1);
	}
	return p;
}

/* just enough of the GL API, all through OSMesaGetProcAddress */
static struct {
	PFNGLGETSTRINGPROC GetString;
	PFNGLCLEARCOLORPROC ClearColor;
	PFNGLCLEARPROC Clear;
	PFNGLVIEWPORTPROC Viewport;
	PFNGLCREATESHADERPROC CreateShader;
	PFNGLSHADERSOURCEPROC ShaderSource;
	PFNGLCOMPILESHADERPROC CompileShader;
	PFNGLGETSHADERIVPROC GetShaderiv;
	PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
	PFNGLCREATEPROGRAMPROC CreateProgram;
	PFNGLATTACHSHADERPROC AttachShader;
	PFNGLLINKPROGRAMPROC LinkProgram;
	PFNGLGETPROGRAMIVPROC GetProgramiv;
	PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
	PFNGLUSEPROGRAMPROC UseProgram;
	PFNGLGENBUFFERSPROC GenBuffers;
	PFNGLBINDBUFFERPROC BindBuffer;
	PFNGLBUFFERDATAPROC BufferData;
	PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
	PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
	PFNGLDRAWARRAYSPROC DrawArrays;
	PFNGLFINISHPROC Finish;
	PFNGLREADPIXELSPROC ReadPixels;
	PFNGLGETERRORPROC GetError;
} gl;

static void
load_gl(void)
{
	gl.GetString = (PFNGLGETSTRINGPROC)sym("glGetString");
	gl.ClearColor = (PFNGLCLEARCOLORPROC)sym("glClearColor");
	gl.Clear = (PFNGLCLEARPROC)sym("glClear");
	gl.Viewport = (PFNGLVIEWPORTPROC)sym("glViewport");
	gl.CreateShader = (PFNGLCREATESHADERPROC)sym("glCreateShader");
	gl.ShaderSource = (PFNGLSHADERSOURCEPROC)sym("glShaderSource");
	gl.CompileShader = (PFNGLCOMPILESHADERPROC)sym("glCompileShader");
	gl.GetShaderiv = (PFNGLGETSHADERIVPROC)sym("glGetShaderiv");
	gl.GetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)sym("glGetShaderInfoLog");
	gl.CreateProgram = (PFNGLCREATEPROGRAMPROC)sym("glCreateProgram");
	gl.AttachShader = (PFNGLATTACHSHADERPROC)sym("glAttachShader");
	gl.LinkProgram = (PFNGLLINKPROGRAMPROC)sym("glLinkProgram");
	gl.GetProgramiv = (PFNGLGETPROGRAMIVPROC)sym("glGetProgramiv");
	gl.GetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)sym("glGetProgramInfoLog");
	gl.UseProgram = (PFNGLUSEPROGRAMPROC)sym("glUseProgram");
	gl.GenBuffers = (PFNGLGENBUFFERSPROC)sym("glGenBuffers");
	gl.BindBuffer = (PFNGLBINDBUFFERPROC)sym("glBindBuffer");
	gl.BufferData = (PFNGLBUFFERDATAPROC)sym("glBufferData");
	gl.VertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)sym("glVertexAttribPointer");
	gl.EnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)sym("glEnableVertexAttribArray");
	gl.DrawArrays = (PFNGLDRAWARRAYSPROC)sym("glDrawArrays");
	gl.Finish = (PFNGLFINISHPROC)sym("glFinish");
	gl.ReadPixels = (PFNGLREADPIXELSPROC)sym("glReadPixels");
	gl.GetError = (PFNGLGETERRORPROC)sym("glGetError");
}

static GLuint
shader(GLenum type, const char *src)
{
	GLint ok = 0;
	GLuint s = gl.CreateShader(type);
	char log[1024];

	gl.ShaderSource(s, 1, &src, NULL);
	gl.CompileShader(s);
	gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		log[0] = 0;
		gl.GetShaderInfoLog(s, sizeof(log), NULL, log);
		LOG("FAIL shader compile: %s", log);
		exit(1);
	}
	return s;
}

/*
 * The kernel's GPU bring-up -- the MMU tables, the case suite and the panel
 * demo -- runs *while* this process is opening its connection: sm_bo_create()
 * answers kIOReturnNotReady until VERSION.ready, and a CREATE_BO made before
 * that is a probe of the flag, not of memory.  (ring-f3i: 8 MiB was accepted
 * because the suite had finished; ring-f3j: the same call ran a second earlier
 * and every size, including 8 MiB, came back an error.)  Wait for a 4 KiB BO
 * to be accepted, and the sizes below describe the allocator.
 */
static int
wait_ready(int fd, int timeout_ms)
{
	struct timespec t0, t1;
	int last = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		struct drm_panfrost_create_bo c;
		struct drm_gem_close g;
		int ms;

		memset(&c, 0, sizeof(c));
		c.size = 4096;
		errno = 0;
		if (drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c) == 0) {
			memset(&g, 0, sizeof(g));
			g.handle = c.handle;
			drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &g);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
			    (t1.tv_nsec - t0.tv_nsec) / 1000000);
			LOG("kernel ready after %d ms", ms);
			return 0;
		}
		last = errno;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
		    (t1.tv_nsec - t0.tv_nsec) / 1000000);
		if (ms >= timeout_ms) {
			LOG("kernel not ready after %d ms (last errno=%d)",
			    ms, last);
			return -1;
		}
		usleep(50000);
	}
}

/*
 * panfrost's device init allocates its tiler heap as one BO of 128 MiB
 * (pan_device.c: "Tiler heap").  The SweetMali driver hands BOs out as single
 * physically contiguous allocations, and iOS has usually fragmented DRAM by
 * the time a LaunchDaemon runs, so a failing CREATE_BO is the first suspect
 * when the screen falls back to softpipe.  Probe the sizes directly, largest
 * first, and close each one again so the real init sees the same memory.
 */
static void
probe_bo(int fd, unsigned mib)
{
	struct drm_panfrost_create_bo c;
	struct drm_gem_close g;
	int rc;

	memset(&c, 0, sizeof(c));
	c.size = (uint32_t)mib << 20;
	errno = 0;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c);
	LOG("probe CREATE_BO %3u MiB: rc=%d errno=%d handle=%u off=0x%llx",
	    mib, rc, errno, c.handle, (unsigned long long)c.offset);
	if (rc != 0)
		return;

	memset(&g, 0, sizeof(g));
	g.handle = c.handle;
	errno = 0;
	rc = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &g);
	LOG("probe CLOSE_BO  %3u MiB: rc=%d errno=%d", mib, rc, errno);
}

/*
 * F3: Mesa's diagnostics go to fd 2, and the only channel that survives to
 * the host is the kernel ring (sweet_drm_log).  A file does not work here:
 * launchd's stderr path cannot be reopened by a root process on this port
 * (open() returns EIO), and even a fresh file could only be read back after
 * the fact.  So stderr becomes the write end of a pipe and a relay thread
 * copies whole lines out of it into the ring as they are produced.
 *
 * Streaming rather than dumping at the end is what makes Mesa's decode of a
 * rejected job chain visible: the ring then shows the submit, the fault and
 * the decoded chain in the order they happened, even if the process dies
 * before main() returns.
 */
static pthread_mutex_t g_relay_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_relay_tid;
static int g_relay_fd = -1;
static int g_sync_seen;

#define RELAY_LINE_MAX  512
#define RELAY_CHUNK     200     /* SM_LOG_MAX is 240 and sweet_drm_log
				 * prefixes "F2: ", so 200 leaves room */
#define RELAY_MARK      "@@SYNC@@\n"

static void relay_finish(void);

/* One line can be longer than a ring entry, so it goes out in chunks. */
static void
relay_emit(const char *s, size_t n)
{
	char chunk[RELAY_CHUNK + 1];

	while (n > 0) {
		size_t take = n > RELAY_CHUNK ? RELAY_CHUNK : n;

		memcpy(chunk, s, take);
		chunk[take] = 0;
		sweet_drm_log("mesa| %s", chunk);
		s += take;
		n -= take;
	}
}

static void *
stderr_relay(void *arg)
{
	char line[RELAY_LINE_MAX];
	char rbuf[256];
	size_t len = 0;
	ssize_t n;

	(void)arg;
	while ((n = read(g_relay_fd, rbuf, sizeof(rbuf))) > 0) {
		ssize_t i;

		for (i = 0; i < n; i++) {
			char c = rbuf[i];

			if (c == '\r') {
				continue;
			}
			if (c != '\n' && len < sizeof(line) - 1) {
				line[len++] = c;
				continue;
			}
			line[len] = 0;
			if (len > 0 && strcmp(line, "@@SYNC@@") == 0) {
				pthread_mutex_lock(&g_relay_lock);
				g_sync_seen = 1;
				pthread_mutex_unlock(&g_relay_lock);
			} else if (len > 0) {
				relay_emit(line, len);
			}
			len = 0;
		}
	}
	if (len > 0) {
		line[len] = 0;
		relay_emit(line, len);
	}
	return NULL;
}

static void
capture_stderr(void)
{
	int pfd[2];

	if (pipe(pfd) != 0) {
		LOG("stderr relay: pipe failed errno=%d", errno);
		return;
	}
	if (dup2(pfd[1], 2) < 0) {
		LOG("stderr relay: dup2 failed errno=%d", errno);
		close(pfd[0]);
		close(pfd[1]);
		return;
	}
	close(pfd[1]);
	/* line buffered, so one fprintf line is one pipe write */
	setvbuf(stderr, NULL, _IOLBF, 0);
	g_relay_fd = pfd[0];
	if (pthread_create(&g_relay_tid, NULL, stderr_relay, NULL) != 0) {
		LOG("stderr relay: pthread_create failed errno=%d", errno);
		close(g_relay_fd);
		g_relay_fd = -1;
		return;
	}
	atexit(relay_finish);
	LOG("stderr relay: fd 2 -> kernel ring");
}

/*
 * Ordering: the pipe is FIFO and the relay walks it in order, so once the
 * relay has seen the marker everything written before it is already in the
 * ring.  That makes the ring readable without a sleep guess.  A lost relay
 * must never hang the test, hence the bounded wait.
 */
static void
relay_sync(void)
{
	struct timespec ts;
	ssize_t w;
	int i;

	if (g_relay_fd < 0) {
		return;
	}
	fflush(stderr);
	w = write(2, RELAY_MARK, strlen(RELAY_MARK));
	(void)w;
	for (i = 0; i < 500; i++) {
		pthread_mutex_lock(&g_relay_lock);
		if (g_sync_seen) {
			g_sync_seen = 0;
			pthread_mutex_unlock(&g_relay_lock);
			return;
		}
		pthread_mutex_unlock(&g_relay_lock);
		ts.tv_sec = 0;
		ts.tv_nsec = 10 * 1000 * 1000;
		nanosleep(&ts, NULL);
	}
	LOG("stderr relay: sync marker lost");
}

static void
relay_finish(void)
{
	if (g_relay_fd < 0) {
		return;
	}
	fflush(stderr);
	close(2);               /* the write end; the relay's read returns 0 */
	pthread_join(g_relay_tid, NULL);
	close(g_relay_fd);
	g_relay_fd = -1;
}

int
main(void)
{
	static const int attribs[] = {
		OSMESA_PROFILE, 0x36,            /* ES profile */
		OSMESA_CONTEXT_MAJOR_VERSION, 3, /* ES 3.0 (API_OPENGLES2) */
		OSMESA_CONTEXT_MINOR_VERSION, 0,
		0
	};
	static const char vs_src[] =
		"attribute vec2 pos;\n"
		"void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
	static const char fs_src[] =
		"precision mediump float;\n"
		"void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
	/* a triangle covering the whole viewport */
	static const float tri[] = {
		-1.0f, -1.0f,
		 3.0f, -1.0f,
		-1.0f,  3.0f,
	};
	unsigned char *buf;
	unsigned char px[4];
	OSMesaContext ctx;
	GLuint prog, vbuf;
	GLint ok = 0;
	int fd;
	char log[1024];

	/*
	 * F3: every ioctl the stand-in translates is logged to the kernel ring
	 * while SWEET_DRM_TRACE is set (connect_once() reads it on the first
	 * connection, so it has to be set before sweet_drm_open()).  A NULL
	 * context or a wrong pixel then comes with a transcript.
	 */
	setenv("SWEET_DRM_TRACE", "1", 1);
	/*
	 * F3 diagnostics: have Mesa decode its own job chains rather than
	 * guessing which kernel structure the hardware rejected.  With TRACE,
	 * pan_jm.c calls pandecode_jc() after every submit and prints the
	 * decoded chain through ctx->dump_stream; PANDECODE_DUMP_FILE=stderr
	 * points that stream at fd 2, which capture_stderr() has turned into a
	 * pipe whose other end a relay thread copies into the kernel ring --
	 * the only channel that survives to the host.
	 *
	 * No "sync": that would make pandecode_abort_on_fault() abort() and
	 * kill the process mid-chain, and the relay only gets what was already
	 * written.  SWEET_DRM_IGNORE_FAULT is what lets Mesa reach the decode
	 * at all -- a refused SUBMIT ioctl makes pan_jm.c return before its
	 * trace block.
	 */
	setenv("PAN_MESA_DEBUG", "trace", 1);
	setenv("PANDECODE_DUMP_FILE", "stderr", 1);
	setenv("SWEET_DRM_IGNORE_FAULT", "1", 1);
	capture_stderr();

	/* bring the stand-in up first so every line below also lands in the
	 * kernel ring even if Mesa never gets as far as opening the device */
	fd = sweet_drm_open();
	if (fd < 0) {
		LOG("FAIL sweet_drm_open");
		return 1;
	}
	raise_memlimit();
	LOG("start pid=%d (OSMesa ES 3.0 -> gallium panfrost -> SweetMali)",
	    (int)getpid());

	wait_ready(fd, 30000);
	probe_bo(fd, 128);
	probe_bo(fd, 64);
	probe_bo(fd, 32);
	probe_bo(fd, 8);

	ctx = OSMesaCreateContextAttribs(attribs, NULL);
	if (ctx == NULL) {
		LOG("FAIL OSMesaCreateContextAttribs returned NULL");
		relay_sync();
		return 1;
	}
	buf = calloc(W * H, 4);
	if (buf == NULL) {
		LOG("FAIL no memory");
		return 1;
	}
	if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, W, H)) {
		LOG("FAIL OSMesaMakeCurrent");
		return 1;
	}
	load_gl();
	LOG("GL_VERSION    = %s", (const char *)gl.GetString(GL_VERSION));
	LOG("GL_RENDERER   = %s", (const char *)gl.GetString(GL_RENDERER));
	LOG("GL_VENDOR     = %s", (const char *)gl.GetString(GL_VENDOR));
	LOG("GL_SL_VERSION = %s",
	    (const char *)gl.GetString(GL_SHADING_LANGUAGE_VERSION));
	relay_sync();

	prog = gl.CreateProgram();
	gl.AttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
	gl.AttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
	gl.LinkProgram(prog);
	gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		log[0] = 0;
		gl.GetProgramInfoLog(prog, sizeof(log), NULL, log);
		LOG("FAIL program link: %s", log);
		return 1;
	}
	gl.UseProgram(prog);

	gl.GenBuffers(1, &vbuf);
	gl.BindBuffer(GL_ARRAY_BUFFER, vbuf);
	gl.BufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
	gl.EnableVertexAttribArray(0);
	gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

	gl.Viewport(0, 0, W, H);
	gl.ClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	gl.Clear(GL_COLOR_BUFFER_BIT);
	gl.DrawArrays(GL_TRIANGLES, 0, 3);
	gl.Finish();

	if (gl.GetError() != GL_NO_ERROR) {
		LOG("FAIL GL error after draw (0x%x)", gl.GetError());
		relay_sync();
		return 1;
	}

	memset(px, 0, sizeof(px));
	gl.ReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	LOG("center pixel = %02x %02x %02x %02x", px[0], px[1], px[2], px[3]);

	/*
	 * The decode of the first job chain, and the descriptor dumps the
	 * stand-in wrote, are all in the ring by now; wait for the relay to
	 * have finished with what is already in the pipe before either exit.
	 */
	relay_sync();

	if (px[0] > 200 && px[1] < 60 && px[2] < 60) {
		LOG("PASS first triangle on the GPU (red center)");
		OSMesaDestroyContext(ctx);
		return 0;
	}
	LOG("FAIL center pixel is not the triangle colour");
	OSMesaDestroyContext(ctx);
	return 1;
}
