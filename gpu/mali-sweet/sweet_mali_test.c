/*
 * sweet_mali_test.c: the first userspace client that submits a job through the
 * libdrm stand-in.
 *
 * F1/B2 proved the client's memory window (MMAP_BO + IOConnectMapMemory64) with
 * a program that called the IOKit user client directly.  F2a is the step Mesa's
 * panfrost driver would take: the same work, but through the *libdrm* entry
 * points (drmOpenWithType, drmGetVersion, drmGetCap, drmIoctl, drmSyncobj*,
 * mmap) that src/panfrost/lib/kmod and src/gallium/drivers/panfrost call.
 * Nothing here is panfrost-specific beyond the ioctl numbers: if this passes,
 * the marshalling the stand-in does is the marshalling Mesa needs.
 *
 * The one check that cannot pass by accident is the last one: the job
 * descriptor is built in BO A by this process, submitted by GPU virtual
 * address, and the GPU writes a magic value into BO B; the value is then read
 * *through the mapping libdrm's mmap returned*.  A bogus mapping (e.g. an
 * interposition that silently did not take, leaving a plain anonymous page)
 * would read back the sentinel instead of the magic, so the test fails rather
 * than passing for the wrong reason.
 *
 *   drmOpenWithType -> drmGetVersion (panfrost 1.2) -> drmGetCap
 *   -> GET_PARAM -> CREATE_BO x2 -> MMAP_BO x2 -> mmap (explicit and
 *   interposed) -> GET_BO_OFFSET -> SYNC_BO -> SYNCOBJ_CREATE -> SUBMIT
 *   -> SYNCCOBJ_WAIT -> WAIT_BO -> SYNC_BO + readback -> CLOSE_BO -> close
 *
 * The verdict goes to IOLog through SM_SEL_LOG (the kernel prints it with its
 * own "SWEETMALI F1:" prefix, since that prefix predates this test) as well as
 * to stdout: the host cannot read the HFS+ root, the console ring is the
 * readback path.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "sweet_mali_abi.h"
#include "sweet_panfrost_ioctl.h"
#include "sweet_drm.h"

#define TEST_VALUE      0xdeadbeefcafef00dULL
#define TEST_SENTINEL   0x1111222233334444ULL
#define JOB_WRITE_VALUE 2u
#define IMM64           7u
#define BO_SIZE         0x1000u

static unsigned g_ok;
static unsigned g_total;

static int
check(int cond, const char *what)
{
	g_total++;
	if (cond) {
		g_ok++;
		sweet_drm_log("ok   %s", what);
	} else {
		sweet_drm_log("BAD  %s (errno=%d %s)", what, errno,
		    strerror(errno));
	}
	return cond;
}

static uint64_t
abs_deadline_ms(int ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec +
	    (uint64_t)ms * 1000000ull;
}

int
main(void)
{
	struct drm_panfrost_get_param gp;
	struct drm_panfrost_create_bo ca;
	struct drm_panfrost_create_bo cb;
	struct drm_panfrost_mmap_bo ma;
	struct drm_panfrost_mmap_bo mb;
	struct drm_panfrost_get_bo_offset ga;
	struct drm_panfrost_get_bo_offset gb;
	struct drm_panfrost_bo_sync_op ops[2];
	struct drm_panfrost_sync_bo sb;
	struct drm_panfrost_submit sub;
	struct drm_panfrost_wait_bo wb;
	drmVersionPtr ver;
	uint64_t cap = 0;
	uint64_t va_a = 0;
	uint64_t va_b = 0;
	uint64_t handle_a = 0;
	uint64_t handle_b = 0;
	uint32_t bo_handles[2];
	uint32_t fence = 0;
	volatile uint32_t *desc = MAP_FAILED;
	volatile uint64_t *target = MAP_FAILED;
	void *map_a = MAP_FAILED;
	void *map_b = MAP_FAILED;
	void *plain_a = MAP_FAILED;
	int fd = -1;
	int aux = -1;
	int ready;
	int rc;

	setvbuf(stdout, NULL, _IONBF, 0);
	sweet_drm_log("start pid=%d libdrm stand-in 1.2", (int)getpid());

	ready = sweet_drm_ready(60000);
	if (!check(ready, "GPU ready (VERSION.ready)")) {
		goto out;
	}

	/* 1: the DRM fd a panfrost screen would open */
	fd = sweet_drm_open();
	if (!check(fd >= 0, "drmOpenWithType(panfrost)")) {
		goto out;
	}

	/* 2: what pan_kmod_drm_drv_match() reads */
	ver = drmGetVersion(fd);
	if (check(ver != NULL, "drmGetVersion") &&
	    check(ver != NULL &&
	    strcmp(ver->name, SWEET_DRM_VERSION_NAME) == 0 &&
	    ver->version_major == SWEET_DRM_VERSION_MAJOR &&
	    ver->version_minor == SWEET_DRM_VERSION_MINOR,
	    "version is panfrost 1.2")) {
		sweet_drm_log("     name=%s %d.%d.%d", ver->name,
		    ver->version_major, ver->version_minor,
		    ver->version_patchlevel);
	}
	drmFreeVersion(ver);

	/* 3: no sync_file on this port, so no timeline (u_sync_provider reads it) */
	rc = drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap);
	check(rc == 0 && cap == 0, "drmGetCap(SYNCOBJ_TIMELINE) == 0");

	/* 4: the GPU, as IOKit knows it */
	memset(&gp, 0, sizeof(gp));
	gp.param = DRM_PANFROST_PARAM_GPU_PROD_ID;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp);
	if (check(rc == 0, "GET_PARAM(GPU_PROD_ID)")) {
		sweet_drm_log("     GPU_PROD_ID=0x%llx (want 0x9093)",
		    (unsigned long long)gp.value);
		check(gp.value == 0x9093ull, "GPU_PROD_ID == 0x9093");
	}

	/* 5/6: two BOs, as panfrost_bo_alloc() would ask for them.  The first
	 * BO a client creates has handle 0 (the kernel hands out table slots),
	 * so only the offsets' agreement with the handles is checked. */
	memset(&ca, 0, sizeof(ca));
	ca.size = BO_SIZE;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &ca);
	if (check(rc == 0, "CREATE_BO A")) {
		handle_a = ca.handle;
		sweet_drm_log("     BO A handle=%llu offset=0x%llx",
		    (unsigned long long)ca.handle,
		    (unsigned long long)ca.offset);
		check(ca.offset == ca.handle, "CREATE_BO A offset == handle");
	}
	memset(&cb, 0, sizeof(cb));
	cb.size = BO_SIZE;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &cb);
	if (check(rc == 0 && cb.handle != ca.handle,
	    "CREATE_BO B (distinct handle)")) {
		handle_b = cb.handle;
		sweet_drm_log("     BO B handle=%llu offset=0x%llx",
		    (unsigned long long)cb.handle,
		    (unsigned long long)cb.offset);
	}

	/* 7/8: the offsets mmap(2) is called with */
	memset(&ma, 0, sizeof(ma));
	ma.handle = (uint32_t)handle_a;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &ma);
	check(rc == 0 && ma.offset == handle_a, "MMAP_BO A offset == handle");
	memset(&mb, 0, sizeof(mb));
	mb.handle = (uint32_t)handle_b;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &mb);
	check(rc == 0 && mb.offset == handle_b, "MMAP_BO B offset == handle");

	/* 9: the explicit entry point */
	map_a = sweet_drm_mmap(fd, BO_SIZE, ma.offset);
	check(map_a != MAP_FAILED, "sweet_drm_mmap(A)");

	/* 9b: BO A through plain mmap(2).  If dyld applied the __interpose
	 * section this is the same memory as map_a, so the sentinel written
	 * through one mapping must be visible through the other.  If it was
	 * not applied, this maps the /dev/null behind the fd and the sentinel
	 * stays local. */
	plain_a = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    (off_t)ma.offset);
	if (check(plain_a != MAP_FAILED, "mmap(A) via __interpose") &&
	    map_a != MAP_FAILED) {
		((volatile uint64_t *)plain_a)[0] = 0xfeedfacecafebeefull;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		check(((volatile uint64_t *)map_a)[0] == 0xfeedfacecafebeefull,
		    "plain mmap(A) is the BO itself");
	}

	/* 10: plain mmap(2) of B, i.e. the way panfrost_bo_map() does it,
	 * through the dyld __interpose section of the stand-in */
	target = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    (off_t)mb.offset);
	check(target != MAP_FAILED, "mmap(B) via __interpose");
	if (target == MAP_FAILED) {
		target = NULL;
	}

	/* 10b: the same B mapping through the explicit entry point.  If this
	 * succeeds while the plain mmap above does not, the kernel's handle-1
	 * mapping is fine and the fault is in the interpose. */
	map_b = sweet_drm_mmap(fd, BO_SIZE, mb.offset);
	check(map_b != MAP_FAILED, "sweet_drm_mmap(B)");

	/* 11/12: the GPU virtual addresses a job descriptor has to name */
	memset(&ga, 0, sizeof(ga));
	ga.handle = (uint32_t)handle_a;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &ga);
	if (check(rc == 0, "GET_BO_OFFSET A")) {
		va_a = ga.offset;
		check(va_a != 0 && (va_a & (SM_BO_PAGE_SIZE - 1u)) == 0,
		    "VA A nonzero, page aligned");
	}
	memset(&gb, 0, sizeof(gb));
	gb.handle = (uint32_t)handle_b;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &gb);
	if (check(rc == 0, "GET_BO_OFFSET B")) {
		va_b = gb.offset;
		check(va_b != 0 && va_b != va_a, "VA B distinct");
	}
	if (va_a == 0 || va_b == 0 || map_a == MAP_FAILED || target == NULL) {
		goto out;
	}

	/*
	 * 13: build the write-value job in BO A.  D2's recipe, and the
	 * descriptor is written by this process, not the kernel: a value that
	 * appears in BO B cannot have come from anything but the GPU.
	 */
	desc = (volatile uint32_t *)map_a;
	{
		unsigned i;

		for (i = 0; i < BO_SIZE / 4u; i++) {
			desc[i] = 0;
		}
		desc[4] = ((JOB_WRITE_VALUE & 0x7fu) << 1) | (1u << 16);
		desc[8] = (uint32_t)va_b;
		desc[9] = (uint32_t)(va_b >> 32);
		desc[10] = IMM64 & 0x7u;
		desc[12] = (uint32_t)TEST_VALUE;
		desc[13] = (uint32_t)(TEST_VALUE >> 32);
		*target = TEST_SENTINEL;
		__sync_synchronize();
	}
	sweet_drm_log("     descriptor at VA 0x%llx, target VA 0x%llx",
	    (unsigned long long)va_a, (unsigned long long)va_b);

	/* 14: SYNC_BO with an ops array, one ioctl for both BOs */
	ops[0].handle = (uint32_t)handle_a;
	ops[0].type = PANFROST_BO_SYNC_CPU_CACHE_FLUSH;
	ops[0].offset = 0;
	ops[0].size = 0;
	ops[1].handle = (uint32_t)handle_b;
	ops[1].type = PANFROST_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE;
	ops[1].offset = 0;
	ops[1].size = 0;
	memset(&sb, 0, sizeof(sb));
	sb.ops = (uint64_t)(uintptr_t)ops;
	sb.op_count = 2;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_SYNC_BO, &sb);
	check(rc == 0, "SYNC_BO flush A + fl/inv B");

	/* 15: the fence the submit will signal.  Handles start at 1 because 0
	 * in a submit means "no fence". */
	rc = drmSyncobjCreate(fd, 0, &fence);
	if (check(rc == 0 && fence != 0, "drmSyncobjCreate")) {
		sweet_drm_log("     fence handle=%u", fence);
	}

	/* 16: submit */
	bo_handles[0] = (uint32_t)handle_a;
	bo_handles[1] = (uint32_t)handle_b;
	memset(&sub, 0, sizeof(sub));
	sub.jc = va_a;
	sub.bo_handles = (uint64_t)(uintptr_t)bo_handles;
	sub.bo_handle_count = 2;
	sub.out_sync = fence;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &sub);
	if (check(rc == 0, "SUBMIT")) {
		uint32_t raw = sweet_drm_last_rawstat();

		sweet_drm_log("     rawstat=0x%x", raw);
		check((raw & 1u) != 0, "rawstat: job done");
		check((raw & (1u << 16)) == 0, "rawstat: no fault");
	}

	/* 17: a submit whose jc is not in a listed BO must be refused */
	bo_handles[0] = (uint32_t)handle_b;
	memset(&sub, 0, sizeof(sub));
	sub.jc = va_a;
	sub.bo_handles = (uint64_t)(uintptr_t)bo_handles;
	sub.bo_handle_count = 1;
	sub.out_sync = 0;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &sub);
	check(rc == -1 && errno == EINVAL, "SUBMIT jc not in listed BO refused");
	bo_handles[0] = (uint32_t)handle_a;

	/* 18: wait on the fence, with DRM's absolute deadline */
	if (fence != 0) {
		rc = drmSyncobjWait(fd, &fence, 1, (int64_t)abs_deadline_ms(2000),
		    0, NULL);
		check(rc == 0, "drmSyncobjWait (absolute timeout)");
	}

	/* 19: WAIT_BO, as panfrost_bo_wait() calls it */
	memset(&wb, 0, sizeof(wb));
	wb.handle = (uint32_t)handle_b;
	wb.timeout_ns = 0;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_WAIT_BO, &wb);
	check(rc == 0, "WAIT_BO");

	/* 20: the payoff -- the GPU's write, read through the mapping */
	memset(&sb, 0, sizeof(sb));
	sb.ops = (uint64_t)(uintptr_t)ops;
	sb.op_count = 1;
	ops[0].handle = (uint32_t)handle_b;
	rc = drmIoctl(fd, DRM_IOCTL_PANFROST_SYNC_BO, &sb);
	__sync_synchronize();
	if (check(rc == 0, "SYNC_BO flush+invalidate B")) {
		uint64_t got = *target;

		sweet_drm_log("     BO B = 0x%llx (want 0x%llx)",
		    (unsigned long long)got,
		    (unsigned long long)TEST_VALUE);
		if (got != TEST_VALUE) {
			sweet_drm_log("     (0x%llx means the mapping is not the BO)",
			    (unsigned long long)TEST_SENTINEL);
		}
		check(got == TEST_VALUE, "GPU wrote through libdrm BO B");
	}

	/* 21: an fd that is not ours is not silently accepted */
	aux = open("/", O_RDONLY);
	if (aux >= 0) {
		int r = drmIoctl(aux, DRM_IOCTL_PANFROST_GET_PARAM, &gp);

		check(r == -1 && errno == ENOTTY, "drmIoctl on a foreign fd: ENOTTY");
	}

out:
	/* Cleanup.  The mappings go first: munmap of a BO is the interposed
	 * IOConnectUnmapMemory64, and closing the BO while it is mapped would
	 * leave the descriptor behind. */
	if (target != NULL && target != MAP_FAILED) {
		(void)munmap((void *)target, BO_SIZE);
	}
	if (map_a != MAP_FAILED) {
		(void)sweet_drm_munmap(map_a, BO_SIZE);
	}
	if (handle_a != 0) {
		(void)drmCloseBufferHandle(fd, (uint32_t)handle_a);
	}
	if (handle_b != 0) {
		(void)drmCloseBufferHandle(fd, (uint32_t)handle_b);
	}
	if (fence != 0) {
		(void)drmSyncobjDestroy(fd, fence);
	}
	if (aux >= 0) {
		(void)close(aux);
	}
	if (fd >= 0) {
		(void)drmClose(fd);
	}

	if (g_total != 0 && g_ok == g_total) {
		sweet_drm_log("PASS %u/%u", g_ok, g_total);
		return 0;
	}
	sweet_drm_log("FAIL %u/%u", g_ok, g_total);
	return 1;
}
