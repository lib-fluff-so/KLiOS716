/*
 * SPDX-License-Identifier: MIT
 *
 * e_test.c: an IOSurface as a GPU render target, from a real process.
 *
 * Stage E's userspace half.  The kernel's own stage E proves the mechanism
 * with a surface it builds through IOCoreSurfaceRoot; this proves the same
 * thing down the path a real client takes:
 *
 *   IOSurface.framework  ->  IOSurfaceRoot (the same door, from a process)
 *   libdrm stand-in      ->  SweetMali user client
 *
 * It creates an IOSurface with IOSurfaceCreate(), asks for that surface's
 * pages in the GPU address space (SM_SEL_SURFACE_BO -- the one call in the ABI
 * with no DRM ioctl behind it), submits a write-value job aimed at the
 * surface's own pixels, and reads the value back out of the IOSurface: through
 * the framework's own base address, and through a second mapping the driver
 * handed out.  Then it does the same for the panel surface (IOSurfaceLookup(1)),
 * whose unscanned tail is where a write is visible without disturbing the
 * picture.
 *
 * What it is for: "render into the surface the compositor is going to scan
 * out".  There is no EGL here yet -- this is the layer an EAGL drawable would
 * sit on, tested on its own, so that a later EGL failure is not confused with
 * the surface handover underneath it.
 *
 * As in the kernel test, the value is what makes the readback mean something: a
 * job that never ran leaves the sentinel in place, so the check that sees the
 * value is the check that saw the GPU write.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
/*
 * The iPhoneOS 17 SDK splits the framework up (IOSurfaceRef.h etc.) and has no
 * IOSurface.h umbrella; older SDKs only have the umbrella.  Either include is
 * fine for the C API, so take whichever this SDK has.
 */
#if __has_include(<IOSurface/IOSurface.h>)
#include <IOSurface/IOSurface.h>
#else
#include <IOSurface/IOSurfaceRef.h>
#endif

#include "sweet_drm.h"
#include "sweet_panfrost_ioctl.h"

/* the geometry the kernel's stage E uses, so the two runs line up */
#define W       256
#define H       256
#define BPR     1024
#define ALLOC   (W * H * 4)
#define OFF     0x100

#define VALUE1  0x0f0f0f0f0f0f0f0full
#define VALUE2  0xf0f0f0f0f0f0f0f0ull
#define SENTINEL 0x1111222233334444ull

#define BGRA    0x42475241u     /* 'BGRA', the surface's own pixel format */

static int ntest, nok, nbad;

#define LOG(...) do { \
	printf("E: " __VA_ARGS__); \
	printf("\n"); \
	fflush(stdout); \
	sweet_drm_log("E: " __VA_ARGS__); \
} while (0)

/*
 * One check: the condition, then the line that says what it found.  The ok/BAD
 * marker is the first argument because it is the first conversion ("%s ") --
 * printf maps arguments to conversions in order, so it cannot come last.
 */
#define CHECK(cond, fmt, ...) do { \
	ntest++; \
	if (cond) { nok++; } else { nbad++; } \
	LOG("%s " fmt, (cond) ? "ok " : "BAD", ##__VA_ARGS__); \
} while (0)

/* ------------------------------------------------------------------ */
/* the DRM-shaped calls the stand-in translates                        */
/* ------------------------------------------------------------------ */

/* drmIoctl() answers -1/errno; every helper below returns 0 or -errno. */
static int
io_rc(void)
{
	return errno != 0 ? -errno : -EIO;
}

static int
bo_create(int fd, uint32_t size, uint32_t *handle, uint64_t *gpu_va)
{
	struct drm_panfrost_create_bo c;

	memset(&c, 0, sizeof(c));
	c.size = size;
	errno = 0;
	if (drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c) != 0) {
		return io_rc();
	}
	*handle = c.handle;
	if (gpu_va != NULL) {
		*gpu_va = c.offset;     /* DRM's `offset` is the GPU VA here */
	}
	return 0;
}

static int
bo_close(int fd, uint32_t handle)
{
	struct drm_gem_close g;

	memset(&g, 0, sizeof(g));
	g.handle = handle;
	errno = 0;
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &g) == 0 ? 0 : io_rc();
}

static int
bo_mmap_offset(int fd, uint32_t handle, uint64_t *offset)
{
	struct drm_panfrost_mmap_bo m;

	memset(&m, 0, sizeof(m));
	m.handle = handle;
	errno = 0;
	if (drmIoctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m) != 0) {
		return io_rc();
	}
	*offset = m.offset;
	return 0;
}

static int
bo_sync(int fd, uint32_t handle, uint32_t type, uint32_t size)
{
	struct drm_panfrost_bo_sync_op op;
	struct drm_panfrost_sync_bo s;

	memset(&op, 0, sizeof(op));
	op.handle = handle;
	op.type = type;
	op.offset = 0;
	op.size = size;
	memset(&s, 0, sizeof(s));
	s.ops = (uint64_t)(uintptr_t)&op;
	s.op_count = 1;
	errno = 0;
	return drmIoctl(fd, DRM_IOCTL_PANFROST_SYNC_BO, &s) == 0 ? 0 : io_rc();
}

static int
bo_submit(int fd, uint64_t jc, uint32_t *bos, uint32_t nbo)
{
	struct drm_panfrost_submit s;

	memset(&s, 0, sizeof(s));
	s.jc = jc;
	s.bo_handles = (uint64_t)(uintptr_t)bos;
	s.bo_handle_count = nbo;
	s.requirements = PANFROST_JD_REQ_FS;
	errno = 0;
	return drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &s) == 0 ? 0 : io_rc();
}

/*
 * The write-value job the kernel's D2/D3/E tests build, word for word: a
 * 4 KiB descriptor block, one job header at word 4, a 64-bit target and a
 * 64-bit value.  It is the smallest thing the fragment slot will run, and it
 * needs no shader -- which is the point at this stage.
 */
static void
job_write_value(volatile uint32_t *desc, uint64_t target, uint64_t value)
{
	unsigned i;

	for (i = 0; i < 4096 / 4; i++) {
		desc[i] = 0;
	}
	desc[4] = ((2u & 0x7fu) << 1) | (1u << 16);     /* WRITE_VALUE */
	desc[8] = (uint32_t)target;
	desc[9] = (uint32_t)(target >> 32);
	desc[10] = 7u & 0x7u;                           /* IMM64 */
	desc[12] = (uint32_t)value;
	desc[13] = (uint32_t)(value >> 32);
}

/* ------------------------------------------------------------------ */
/* IOSurface creation                                                  */
/* ------------------------------------------------------------------ */

static CFNumberRef
num32(int32_t v)
{
	return CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
}

/*
 * The five keys the kernel's SweetSurface reads for a simple single-plane
 * buffer, under the framework's own names.  IOSurface.framework serializes
 * this dictionary to the kext, which is why the names here are the kext's.
 */
static IOSurfaceRef
surface_create(unsigned w, unsigned h, unsigned bpr, uint32_t fmt)
{
	CFMutableDictionaryRef d;
	IOSurfaceRef s;

	d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (d == NULL) {
		return NULL;
	}
#define PUT(key, val) do { \
	CFNumberRef n_ = num32((int32_t)(val)); \
	if (n_ != NULL) { \
		CFDictionarySetValue(d, (key), n_); \
		CFRelease(n_); \
	} \
} while (0)
	PUT(kIOSurfaceWidth, w);
	PUT(kIOSurfaceHeight, h);
	PUT(kIOSurfaceBytesPerElement, 4);
	PUT(kIOSurfaceBytesPerRow, bpr);
	PUT(kIOSurfacePixelFormat, fmt);
#undef PUT

	s = IOSurfaceCreate(d);
	CFRelease(d);
	return s;
}

int
main(void)
{
	uint32_t bos[2];
	struct sm_surface_bo sb;
	IOSurfaceRef surf, panel;
	IOSurfaceID id = 0, pid = 0;
	volatile uint32_t *desc;
	volatile uint64_t *pix;
	void *base = NULL, *win2 = NULL, *desc_cpu = NULL;
	uint32_t h_desc = 0, h_surf = 0, h_panel = 0;
	uint64_t va_desc = 0, va_surf = 0, va_panel = 0, off = 0;
	uint64_t panel_alloc = 0, panel_tail_off = 0;
	volatile uint64_t *ptail = NULL;
	int fd, rc;

	/* the stand-in logs every ioctl it translates (see f3_test.c) */
	setenv("SWEET_DRM_TRACE", "1", 1);

	fd = sweet_drm_open();
	if (fd < 0) {
		LOG("FAIL sweet_drm_open");
		return 1;
	}
	LOG("start pid=%d (process: IOSurface.framework -> SweetMali)",
	    (int)getpid());
	if (!sweet_drm_ready(30000)) {
		LOG("FAIL kernel not ready after 30000 ms");
		return 1;
	}
	LOG("kernel ready");

	/* 1: the surface, created the way an app or the compositor would */
	surf = surface_create(W, H, BPR, BGRA);
	if (surf != NULL) {
		id = IOSurfaceGetID(surf);
		base = IOSurfaceGetBaseAddress(surf);
	}
	CHECK(surf != NULL && id != 0 && base != NULL,
	    "IOSurfaceCreate id=%u %zux%zu bpr=%zu base=%p",
	    id, IOSurfaceGetWidth(surf), IOSurfaceGetHeight(surf),
	    IOSurfaceGetBytesPerRow(surf), base);
	if (surf == NULL || id == 0 || base == NULL) {
		LOG("FAIL no surface to work with");
		return 1;
	}

	/* 2: its geometry, without making a BO out of it */
	memset(&sb, 0, sizeof(sb));
	sb.surface_id = id;
	sb.handle = SM_SURFACE_DESCRIBE_ONLY;
	rc = sweet_drm_surface_bo(&sb);
	CHECK(rc == 0 && sb.size == ALLOC && sb.width == W && sb.height == H &&
	    sb.bytes_per_row == BPR && sb.pixel_format == BGRA &&
	    sb.gpu_va == 0,
	    "SURFACE_BO describe: rc=%d size=%llu %ux%u bpr=%u fmt=0x%x va=%llx",
	    rc, (unsigned long long)sb.size, sb.width, sb.height,
	    sb.bytes_per_row, sb.pixel_format, (unsigned long long)sb.gpu_va);

	/* 3: the surface's own pages, in the GPU address space */
	memset(&sb, 0, sizeof(sb));
	sb.surface_id = id;
	rc = sweet_drm_surface_bo(&sb);
	h_surf = sb.handle;
	va_surf = sb.gpu_va;
	CHECK(rc == 0 && va_surf != 0 && sb.size == ALLOC,
	    "SURFACE_BO: rc=%d handle=%u va=%llx size=%llu",
	    rc, h_surf, (unsigned long long)va_surf,
	    (unsigned long long)sb.size);
	if (rc != 0 || va_surf == 0) {
		LOG("FAIL no GPU mapping for the surface");
		return 1;
	}

	/* 4: the job's descriptor, a BO the CPU writes and the GPU reads */
	rc = bo_create(fd, 4096, &h_desc, &va_desc);
	CHECK(rc == 0 && va_desc != 0,
	    "CREATE_BO descriptor: rc=%d handle=%u va=%llx",
	    rc, h_desc, (unsigned long long)va_desc);
	rc = bo_mmap_offset(fd, h_desc, &off);
	desc_cpu = rc == 0 ? sweet_drm_mmap(fd, 4096, off) : NULL;
	CHECK(desc_cpu != NULL && desc_cpu != MAP_FAILED,
	    "descriptor window: rc=%d off=%llu cpu=%p", rc,
	    (unsigned long long)off, desc_cpu);
	if (desc_cpu == NULL || desc_cpu == MAP_FAILED) {
		LOG("FAIL no descriptor window");
		return 1;
	}
	desc = (volatile uint32_t *)desc_cpu;

	/* 5: a write-value job aimed at the surface's own pixels */
	pix = (volatile uint64_t *)((char *)base + OFF);
	job_write_value(desc, va_surf + OFF, VALUE1);
	*pix = SENTINEL;
	bo_sync(fd, h_desc, PANFROST_BO_SYNC_CPU_CACHE_FLUSH, 4096);
	bos[0] = h_desc;
	bos[1] = h_surf;
	rc = bo_submit(fd, va_desc, bos, 2);
	bo_sync(fd, h_surf, PANFROST_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE,
	    (uint32_t)sb.size);
	CHECK(rc == 0 && *pix == VALUE1,
	    "SUBMIT -> surface[0x%x]=%llx want=%llx rc=%d",
	    OFF, (unsigned long long)*pix, (unsigned long long)VALUE1, rc);

	/* 6: a second mapping of the same surface -- the compositor's view */
	rc = bo_mmap_offset(fd, h_surf, &off);
	win2 = rc == 0 ? sweet_drm_mmap(fd, (size_t)sb.size, off) : NULL;
	CHECK(win2 != NULL && win2 != MAP_FAILED &&
	    *(volatile uint64_t *)((char *)win2 + OFF) == VALUE1,
	    "second mapping sees the GPU's value: rc=%d off=%llu win=%p",
	    rc, (unsigned long long)off, win2);

	/* 7: the same job again, now read through the framework's address */
	job_write_value(desc, va_surf + OFF, VALUE2);
	bo_sync(fd, h_desc, PANFROST_BO_SYNC_CPU_CACHE_FLUSH, 4096);
	rc = bo_submit(fd, va_desc, bos, 2);
	bo_sync(fd, h_surf, PANFROST_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE,
	    (uint32_t)sb.size);
	CHECK(rc == 0 && *pix == VALUE2 && win2 != NULL &&
	    win2 != MAP_FAILED &&
	    *(volatile uint64_t *)((char *)win2 + OFF) == VALUE2,
	    "second SUBMIT -> surface[0x%x]=%llx want=%llx rc=%d",
	    OFF, (unsigned long long)*pix, (unsigned long long)VALUE2, rc);

	/*
	 * 8: closing the BO takes the GPU mapping away, not the surface.  The
	 * pixels are the IOSurface's, and the compositor's buffer is not the
	 * driver's to free -- the kernel test checks the PTE side from inside,
	 * this checks the client's side from outside.
	 */
	rc = bo_close(fd, h_surf);
	CHECK(rc == 0 && *pix == VALUE2,
	    "CLOSE_BO surface: rc=%d, pixels survive: surface[0x%x]=%llx",
	    rc, OFF, (unsigned long long)*pix);

	/* a submit that names the closed handle must be refused */
	rc = bo_submit(fd, va_desc, bos, 2);
	CHECK(rc == -EINVAL, "SUBMIT with the closed handle: rc=%d (want %d)",
	    rc, -EINVAL);

	/* 9: the panel surface, by id -- the one the display scans out */
	panel = IOSurfaceLookup(1);
	if (panel != NULL) {
		pid = IOSurfaceGetID(panel);
		panel_alloc = IOSurfaceGetAllocSize(panel);
	}
	CHECK(panel != NULL && pid == 1 && panel_alloc != 0,
	    "IOSurfaceLookup(1): %p id=%u %zux%zu alloc=%llu",
	    panel, pid, panel != NULL ? IOSurfaceGetWidth(panel) : 0,
	    panel != NULL ? IOSurfaceGetHeight(panel) : 0,
	    (unsigned long long)panel_alloc);

	if (panel != NULL && pid == 1) {
		memset(&sb, 0, sizeof(sb));
		sb.surface_id = pid;
		rc = sweet_drm_surface_bo(&sb);
		h_panel = sb.handle;
		va_panel = sb.gpu_va;
		CHECK(rc == 0 && va_panel != 0 && sb.size == panel_alloc,
		    "panel as BO: rc=%d handle=%u va=%llx size=%llu "
		    "(surface %llu)",
		    rc, h_panel, (unsigned long long)va_panel,
		    (unsigned long long)sb.size,
		    (unsigned long long)panel_alloc);

		/*
		 * The tail: the last bytes of the allocation that the display
		 * never scans (row alignment padding).  Writing there is a GPU
		 * write into the panel's own memory that cannot change the
		 * picture -- which is what makes this safe to do on a running
		 * system.
		 */
		panel_tail_off = panel_alloc - 8;
		if (rc == 0 && va_panel != 0 && panel_tail_off > 0) {
			ptail = (volatile uint64_t *)((char *)
			    IOSurfaceGetBaseAddress(panel) + panel_tail_off);
			job_write_value(desc, va_panel + panel_tail_off, VALUE1);
			*ptail = SENTINEL;
			bo_sync(fd, h_desc,
			    PANFROST_BO_SYNC_CPU_CACHE_FLUSH, 4096);
			bos[0] = h_desc;
			bos[1] = h_panel;
			rc = bo_submit(fd, va_desc, bos, 2);
			bo_sync(fd, h_panel,
			    PANFROST_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE,
			    (uint32_t)sb.size);
			CHECK(rc == 0 && ptail != NULL && *ptail == VALUE1,
			    "SUBMIT -> panel tail[%llu]=%llx want=%llx rc=%d",
			    (unsigned long long)panel_tail_off,
			    ptail != NULL ? (unsigned long long)*ptail : 0,
			    (unsigned long long)VALUE1, rc);
			bo_close(fd, h_panel);
		}
	}

	LOG("%s (%d/%d)", nbad == 0 ? "PASS" : "FAIL", nok, ntest);
	return nbad == 0 ? 0 : 1;
}
