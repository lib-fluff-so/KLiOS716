/*
 * sweet_drm.c: libdrm stand-in for Mesa's panfrost driver on KL7 (MT6789, Mali-G57).
 *
 * Mesa's panfrost backend talks to /dev/dri/renderD* the way every DRM driver
 * does: through libdrm (drmIoctl, drmGetVersion, drmSyncobj*) and by mmap()ing
 * BOs at the offset DRM_IOCTL_PANFROST_MMAP_BO returned.  On KL7 there is no
 * DRM: the same requests are selectors of the SweetMali IOKit user client
 * (bsd/sys/sweet_mali_abi.h), so this file provides, *under the libdrm names*,
 * exactly what the backend calls, and forwards each request to
 * IOConnectCallStructMethod.  No Mesa source change is needed -- which is the
 * requirement, since Mesa's AGENTS.md forbids editing src/panfrost.
 *
 *  - "device fds" are opens of /dev/null (so dup, close, fstat, os_dupfd_cloexec
 *    all keep working); any such fd means THE connection, because there is one
 *    GPU and one connection per process;
 *  - mmap()/munmap() of such an fd are interposed (dyld __interpose) and turned
 *    into IOConnectMapMemory64/IOConnectUnmapMemory64 with the BO handle as the
 *    memory type (MMAP_BO's offset *is* the handle); sweet_drm_mmap() is the
 *    same call for callers that would rather be explicit;
 *  - the DRM structs and the kernel's are not always the same shape, so every
 *    call is marshalled field by field -- drm_panfrost_create_bo puts `offset`
 *    last, sm_create_bo puts it second, and drm_panfrost_submit passes
 *    pointers where sm_submit carries inline arrays;
 *  - DRM's absolute CLOCK_MONOTONIC timeouts (drmSyncobjWait, wait_bo) become
 *    the kernel's relative ones;
 *  - the reported DRM version is panfrost 1.2.  1.1 is the minimum panfrost's
 *    userspace accepts; 1.3+ makes it query SYSTEM_TIMESTAMP*_FREQUENCY, which
 *    this kernel answers Unsupported (and a required param that fails is an
 *    assert, or a silent 0 with NDEBUG), so 1.2 is the newest version whose
 *    userspace paths all land on calls the driver implements.
 *
 * What this file deliberately does not do: emulate a sync_file.  Mesa's
 * panfrost gallium turns a fence into an fd with drmSyncobjExportSyncFile()
 * (pan_fence.c) and imports fds with drmSyncobjImportSyncFile(); a real
 * sync_file needs a kernel object behind the fd.  Until a winsys decides what
 * to do about that (F2b, see README.md), Export/Import and the prime calls
 * answer ENOSYS loudly instead of handing back an fd nothing can wait on.
 *
 * Build: gpu/mali-sweet/build.sh (arm64 dylib, install name libdrm.2.dylib so
 * a Mesa linked against a libdrm of that name uses it).
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sweet_mali_abi.h"
#include "sweet_panfrost_ioctl.h"
#include "sweet_drm.h"

#define EXPORT __attribute__((visibility("default")))

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t g_conn;
static dev_t g_null_rdev;
static int g_trace;
static int g_ignore_fault;
static uint32_t g_last_rawstat;

/* Offset applied to a DRM sync object handle to get the kernel's.  Both sides
 * number from 1, so it is zero; it exists so the one place that must never
 * treat 0 as "the first object" is visible in one line. */
#define SM_SYNCOBJ_OFFSET 0

/*
 * F3 diagnostics: the buffers a client has allocated, with the GPU address
 * the kernel chose (CREATE_BO) and the CPU address the client mapped on
 * MMAP_BO's offset.  With both, a refused SUBMIT can be decoded here rather
 * than in the kernel: which BO the job chain lives in, what the hardware
 * wrote over the job header as its exception record, and -- the part that
 * names the bug -- which BO the fault pointer points into.
 *
 * A zero handle is a free slot.  The table is deliberately not reaped on
 * mmap failure: a BO whose CPU mapping failed is still worth reporting.
 */
#define SM_DIAG_BO_MAX 512
static struct {
	uint32_t handle;
	uint32_t size;
	uint64_t gpu_va;
	void *cpu;
} g_diag_bos[SM_DIAG_BO_MAX];

static void
diag_bo_note_create(uint32_t handle, uint32_t size, uint64_t gpu_va)
{
	unsigned i;

	for (i = 0; i < SM_DIAG_BO_MAX; i++) {
		if (g_diag_bos[i].handle == 0 ||
		    g_diag_bos[i].handle == handle) {
			g_diag_bos[i].handle = handle;
			g_diag_bos[i].size = size;
			g_diag_bos[i].gpu_va = gpu_va;
			g_diag_bos[i].cpu = NULL;
			return;
		}
	}
}

static void
diag_bo_note_mmap(uint32_t handle, void *cpu)
{
	unsigned i;

	for (i = 0; i < SM_DIAG_BO_MAX; i++) {
		if (g_diag_bos[i].handle == handle) {
			g_diag_bos[i].cpu = cpu;
			return;
		}
	}
}

static void
diag_bo_note_close(uint32_t handle)
{
	unsigned i;

	for (i = 0; i < SM_DIAG_BO_MAX; i++) {
		if (g_diag_bos[i].handle == handle) {
			memset(&g_diag_bos[i], 0, sizeof(g_diag_bos[i]));
			return;
		}
	}
}

/* Index of the BO containing `va`, or -1; `*off` is the byte offset in it. */
static int
diag_bo_find_va(uint64_t va, uint64_t *off)
{
	unsigned i;

	*off = 0;
	for (i = 0; i < SM_DIAG_BO_MAX; i++) {
		if (g_diag_bos[i].handle == 0) {
			continue;
		}
		if (va >= g_diag_bos[i].gpu_va &&
		    va < g_diag_bos[i].gpu_va + g_diag_bos[i].size) {
			*off = va - g_diag_bos[i].gpu_va;
			return (int)i;
		}
	}
	return -1;
}

/* Which kernel selector stands in for which ioctl number. */
static size_t
sel_size(uint32_t sel)
{
	switch (sel) {
	case SM_SEL_VERSION:        return sizeof(struct sm_abi_info);
	case SM_SEL_GET_PARAM:      return sizeof(struct sm_get_param);
	case SM_SEL_CREATE_BO:      return sizeof(struct sm_create_bo);
	case SM_SEL_GET_BO_OFFSET:  return sizeof(struct sm_get_bo_offset);
	case SM_SEL_CLOSE_BO:       return sizeof(struct sm_close_bo);
	case SM_SEL_SUBMIT:         return sizeof(struct sm_submit);
	case SM_SEL_WAIT_BO:        return sizeof(struct sm_wait_bo);
	case SM_SEL_SYNC_BO:        return sizeof(struct sm_sync_bo);
	case SM_SEL_SYNCOBJ_CREATE: return sizeof(struct sm_syncobj_create);
	case SM_SEL_SYNCOBJ_DESTROY: return sizeof(struct sm_syncobj_destroy);
	case SM_SEL_SYNCOBJ_WAIT:   return sizeof(struct sm_syncobj_wait);
	case SM_SEL_MMAP_BO:        return sizeof(struct sm_mmap_bo);
	case SM_SEL_LOG:            return sizeof(struct sm_log);
	case SM_SEL_SURFACE_BO:     return sizeof(struct sm_surface_bo);
	default:                    return 0;
	}
}

static int
io_errno(kern_return_t kr)
{
	switch (kr) {
	case kIOReturnSuccess:      return 0;
	case kIOReturnNoMemory:
	case kIOReturnNoSpace:
	case kIOReturnNoResources:  return ENOMEM;
	case kIOReturnBadArgument:  return EINVAL;
	case kIOReturnTimeout:      return ETIMEDOUT;
	case kIOReturnBusy:         return EBUSY;
	case kIOReturnNotReady:
	case kIOReturnNotFound:     return ENODEV;
	case kIOReturnUnsupported:  return ENOSYS;
	case kIOReturnNotPermitted: return EPERM;
	case kIOReturnNoDevice:     return ENODEV;
	default:                    return EIO;
	}
}

/*
 * Both signs reach the places that turn a translator's answer into a syscall
 * result: the sweet_call*() family hands back io_errno()'s positive value,
 * while a translator's own refusals are negative (-EINVAL in sm_submit()).
 * errno is positive by definition, so a negative one is not merely ugly -- it
 * makes the caller's `-errno` come out positive, which is how the E2 check on
 * a SUBMIT naming a closed handle read +22 where the DRM convention says -22
 * (ring-e2g).  One function, used at every boundary, so the two conventions
 * cannot disagree again.
 */
static int
errno_value(int ret)
{
	return ret < 0 ? -ret : ret;
}

/*
 * One connection per process, opened on first use.  The service is published
 * before the driver's bring-up finishes, so the opening itself never has to
 * wait -- VERSION.ready is what says the GPU is up.
 */
static int
connect_once(void)
{
	int waited = 0;
	int timeout_ms = 60000;

	pthread_mutex_lock(&g_lock);
	if (g_conn == IO_OBJECT_NULL) {
		struct stat st;
		const char *env = getenv("SWEET_DRM_OPEN_TIMEOUT_MS");
		int v;

		if (stat("/dev/null", &st) == 0) {
			g_null_rdev = st.st_rdev;
		}
		g_trace = getenv("SWEET_DRM_TRACE") != NULL;
		/*
		 * F3 diagnostics: when set, a SUBMIT the kernel refused is
		 * reported to Mesa as success.  pan_jm.c returns before its
		 * trace/decode block on a failing ioctl, so without this the
		 * decode of the bad chain never runs.
		 */
		env = getenv("SWEET_DRM_IGNORE_FAULT");
		g_ignore_fault = env != NULL && atoi(env) != 0;
		env = getenv("SWEET_DRM_OPEN_TIMEOUT_MS");
		if (env != NULL && (v = atoi(env)) > 0) {
			timeout_ms = v;
		}
	}
	while (g_conn == IO_OBJECT_NULL) {
		io_service_t svc;
		CFMutableDictionaryRef match = IOServiceMatching(SM_SERVICE_NAME);

		if (match == NULL) {
			break;
		}
		svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
		if (svc != IO_OBJECT_NULL) {
			if (IOServiceOpen(svc, mach_task_self(), 0, &g_conn)
			    != KERN_SUCCESS) {
				g_conn = IO_OBJECT_NULL;
			}
			IOObjectRelease(svc);
			break;
		}
		if (waited >= timeout_ms) {
			break;
		}
		/* The service is published by the driver; a client that starts
		 * first waits for it rather than failing. */
		pthread_mutex_unlock(&g_lock);
		usleep(100000);
		waited += 100;
		pthread_mutex_lock(&g_lock);
	}
	pthread_mutex_unlock(&g_lock);
	return g_conn != IO_OBJECT_NULL;
}

/*
 * Is this fd one of ours?  The stand-in hands out /dev/null opens, so the
 * object's rdev is the test; the connection having been made is what keeps a
 * process that never called drmOpenWithType() from being fooled by it.
 */
static int
is_ours(int fd)
{
	struct stat st;

	if (g_conn == IO_OBJECT_NULL || fd < 0 || fstat(fd, &st) != 0) {
		return 0;
	}
	return S_ISCHR(st.st_mode) && st.st_rdev == g_null_rdev;
}

static kern_return_t
sweet_call(uint32_t sel, const void *in, size_t insz, void *out,
    size_t *outsz)
{
	if (!connect_once()) {
		return kIOReturnNotReady;
	}
	return IOConnectCallStructMethod(g_conn, sel, in, insz, out, outsz);
}

/* in-out call: the same buffer in and out, exactly sel_size(sel) bytes. */
static int
sweet_call_io(uint32_t sel, void *buf)
{
	size_t sz = sel_size(sel);
	size_t outsz = sz;
	kern_return_t kr;

	if (sz == 0) {
		return -EINVAL;
	}
	kr = sweet_call(sel, buf, sz, buf, &outsz);
	return io_errno(kr);
}

static int
sweet_call_in(uint32_t sel, const void *buf)
{
	size_t sz = sel_size(sel);

	if (sz == 0) {
		return -EINVAL;
	}
	return io_errno(sweet_call(sel, buf, sz, NULL, NULL));
}

EXPORT void
sweet_drm_log(const char *fmt, ...)
{
	char buf[SM_LOG_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	printf("F2: %s\n", buf);
	fflush(stdout);
	if (g_conn != IO_OBJECT_NULL) {
		struct sm_log l;
		size_t outsz = sizeof(l);

		memset(&l, 0, sizeof(l));
		/* keep room for the two-character stage prefix */
		snprintf(l.text, sizeof(l.text), "F2: %s", buf);
		l.size = (uint32_t)strlen(l.text);
		(void)IOConnectCallStructMethod(g_conn, SM_SEL_LOG, &l,
		    sizeof(l), &l, &outsz);
	}
}

EXPORT int
sweet_drm_trace(void)
{
	return g_trace;
}

/*
 * E: an IOSurface's pixels as a BO (SM_SEL_SURFACE_BO).
 *
 * This is the one selector a client reaches through this library rather than
 * through an ioctl, because it has no DRM ioctl to stand in for: DRM's answer
 * to "render into this surface" is a dmabuf/prime fd, and this port has no
 * DRM node.  The call is a straight marshalling of the struct, and the handle
 * that comes back is an ordinary BO handle: SUBMIT, SYNC_BO and MMAP_BO take
 * it exactly as they take a CREATE_BO one.
 *
 * `sb->surface_id` must already be a live IOSurface (IOSurfaceCreate() /
 * IOSurfaceLookup()) in *this* process's IOSurface table -- the kernel holds
 * the surface's pages in the GPU address space for as long as the handle is
 * open, and refuses an id it does not know.  On success `sb->handle` and
 * `sb->gpu_va` are answers; the geometry is filled on both this path and the
 * SM_SURFACE_DESCRIBE_ONLY one.
 *
 * Handle 0 is legal here (handles are slot indices), so a caller that wants to
 * know whether it got a BO must check the return value, not the handle.
 */
EXPORT int
sweet_drm_surface_bo(struct sm_surface_bo *sb)
{
	size_t sz = sizeof(*sb);

	if (sb == NULL) {
		return -EINVAL;
	}
	return io_errno(sweet_call(SM_SEL_SURFACE_BO, sb, sz, sb, &sz));
}

EXPORT int
sweet_drm_info(struct sm_abi_info *out)
{
	size_t outsz = sizeof(*out);

	memset(out, 0, sizeof(*out));
	return io_errno(sweet_call(SM_SEL_VERSION, NULL, 0, out, &outsz));
}

EXPORT int
sweet_drm_ready(int timeout_ms)
{
	struct sm_abi_info info;
	int waited = 0;

	for (;;) {
		if (sweet_drm_info(&info) == 0 && info.ready != 0) {
			return 1;
		}
		if (waited >= timeout_ms) {
			return 0;
		}
		usleep(100000);
		waited += 100;
	}
}

EXPORT uint32_t
sweet_drm_last_rawstat(void)
{
	return g_last_rawstat;
}

/* ------------------------------------------------------------------ */
/* wrappers, one per selector                                          */
/* ------------------------------------------------------------------ */

static int
sm_get_param(uint32_t param, uint64_t *value)
{
	struct sm_get_param p;
	int ret;

	memset(&p, 0, sizeof(p));
	p.param = param;
	ret = sweet_call_io(SM_SEL_GET_PARAM, &p);
	if (ret == 0 && value != NULL) {
		*value = p.value;
	}
	return ret;
}

/*
 * F3: `gpu_va` is the address the kernel mapped the BO at, and it is the one
 * value a panfrost client cannot do without -- see the struct's comment.  The
 * ABI's `offset` (the CPU mmap offset, equal to the handle) is still returned
 * here because MMAP_BO's answer and this one are the same number in D2's
 * design; the caller only needs the handle for mmap.
 */
static int
sm_create_bo(uint32_t size, uint32_t *handle, uint64_t *offset,
    uint64_t *gpu_va)
{
	struct sm_create_bo c;
	int ret;

	memset(&c, 0, sizeof(c));
	c.size = size;
	c.flags = 0;
	ret = sweet_call_io(SM_SEL_CREATE_BO, &c);
	if (ret == 0) {
		diag_bo_note_create(c.handle, c.size, c.gpu_va);
		if (g_trace) {
			sweet_drm_log("CREATE_BO h=%u va=%#llx size=%u off=%llu",
			    c.handle, (unsigned long long)c.gpu_va, c.size,
			    (unsigned long long)c.offset);
		}
		if (handle != NULL) {
			*handle = c.handle;
		}
		if (offset != NULL) {
			*offset = c.offset;
		}
		if (gpu_va != NULL) {
			*gpu_va = c.gpu_va;
		}
	}
	return ret;
}

static int
sm_get_bo_offset(uint32_t handle, uint64_t *va)
{
	struct sm_get_bo_offset g;
	int ret;

	memset(&g, 0, sizeof(g));
	g.handle = handle;
	ret = sweet_call_io(SM_SEL_GET_BO_OFFSET, &g);
	if (ret == 0 && va != NULL) {
		*va = g.offset;
	}
	return ret;
}

static int
sm_close_bo(uint32_t handle)
{
	struct sm_close_bo c;

	memset(&c, 0, sizeof(c));
	c.handle = handle;
	diag_bo_note_close(handle);
	return sweet_call_in(SM_SEL_CLOSE_BO, &c);
}

static int
sm_mmap_bo(uint32_t handle, uint64_t *offset)
{
	struct sm_mmap_bo m;
	int ret;

	memset(&m, 0, sizeof(m));
	m.handle = handle;
	ret = sweet_call_io(SM_SEL_MMAP_BO, &m);
	if (ret == 0 && offset != NULL) {
		*offset = m.offset;
	}
	return ret;
}

static int
sm_sync_bo(uint32_t handle, uint32_t type, uint32_t offset, uint32_t size)
{
	struct sm_sync_bo s;

	memset(&s, 0, sizeof(s));
	s.handle = handle;
	s.type = type;
	s.offset = offset;
	s.size = size;
	return sweet_call_in(SM_SEL_SYNC_BO, &s);
}

/* timeout_ns is relative here, as the ABI says. */
static int
sm_wait_bo(uint32_t handle, int64_t timeout_ns, uint32_t *signaled)
{
	struct sm_wait_bo w;
	int ret;

	memset(&w, 0, sizeof(w));
	w.handle = handle;
	w.timeout_ns = timeout_ns;
	ret = sweet_call_io(SM_SEL_WAIT_BO, &w);
	if (ret == 0) {
		if (signaled != NULL) {
			*signaled = w.signaled;
		}
		if (w.signaled == 0) {
			ret = -ETIMEDOUT;
		}
	}
	return ret;
}

static int
sm_syncobj_create(uint32_t flags, uint32_t *handle)
{
	struct sm_syncobj_create c;
	int ret;

	memset(&c, 0, sizeof(c));
	c.flags = flags & SM_SYNCOBJ_CREATE_SIGNALED;
	ret = sweet_call_io(SM_SEL_SYNCOBJ_CREATE, &c);
	if (ret == 0) {
		*handle = c.handle + SM_SYNCOBJ_OFFSET;
	}
	return ret;
}

static int
sm_syncobj_destroy(uint32_t handle)
{
	struct sm_syncobj_destroy d;

	memset(&d, 0, sizeof(d));
	d.handle = handle - SM_SYNCOBJ_OFFSET;
	return sweet_call_in(SM_SEL_SYNCOBJ_DESTROY, &d);
}

static int
sm_syncobj_wait(uint32_t handle, int64_t timeout_ns, uint32_t *signaled)
{
	struct sm_syncobj_wait w;
	int ret;

	memset(&w, 0, sizeof(w));
	w.handle = handle - SM_SYNCOBJ_OFFSET;
	w.timeout_ns = timeout_ns;
	ret = sweet_call_io(SM_SEL_SYNCOBJ_WAIT, &w);
	if (ret == 0) {
		if (signaled != NULL) {
			*signaled = w.signaled;
		}
		if (w.signaled == 0) {
			ret = -ETIMEDOUT;
		}
	}
	return ret;
}

/*
 * The job header is built by the client inside one of the BOs (D2 proved that
 * path); SUBMIT's job front-end reads it through the MMU.  Of panfrost's
 * fields only `jm_ctx_handle` is dropped (the kernel has no scheduler and no
 * JM context); `requirements` is forwarded, because on this hardware the slot
 * is not free: PANFROST_JD_REQ_FS selects the fragment slot and a vertex or
 * tiler job must go to the other one, or the hardware refuses the chain with
 * DATA_INVALID_FAULT.
 */
static int
sm_submit(const struct drm_panfrost_submit *s)
{
	struct sm_submit sub;
	const uint32_t *bos = (const uint32_t *)(uintptr_t)s->bo_handles;
	const uint32_t *ins = (const uint32_t *)(uintptr_t)s->in_syncs;
	uint32_t i;
	int ret;

	if (s->bo_handle_count == 0 ||
	    s->bo_handle_count > SM_SUBMIT_MAX_BOS ||
	    s->in_sync_count > SM_SUBMIT_MAX_SYNCS) {
		return -EINVAL;
	}
	memset(&sub, 0, sizeof(sub));
	sub.jc = s->jc;
	sub.bo_count = s->bo_handle_count;
	sub.requirements = s->requirements;
	for (i = 0; i < s->bo_handle_count; i++) {
		if (bos == NULL) {
			return -EINVAL;
		}
		sub.bo_handles[i] = bos[i];
	}
	sub.out_sync = s->out_sync == 0 ? 0 :
	    (uint64_t)(s->out_sync - SM_SYNCOBJ_OFFSET);
	sub.in_sync_count = s->in_sync_count;
	for (i = 0; i < s->in_sync_count; i++) {
		if (ins == NULL) {
			return -EINVAL;
		}
		sub.in_syncs[i] = ins[i] - SM_SYNCOBJ_OFFSET;
	}
	ret = sweet_call_io(SM_SEL_SUBMIT, &sub);
	g_last_rawstat = sub.rawstat;
	return ret;
}

/* ------------------------------------------------------------------ */
/* the ioctl entry point                                               */
/* ------------------------------------------------------------------ */

static uint64_t
mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* DRM passes an absolute CLOCK_MONOTONIC deadline; the kernel wants a delta. */
static int64_t
abs_to_rel_ns(uint64_t abs_ns)
{
	uint64_t now;

	if (abs_ns == 0) {
		return 0;                       /* poll */
	}
	if (abs_ns == UINT64_MAX) {
		return SM_WAIT_INFINITE;
	}
	now = mono_ns();
	if (abs_ns <= now) {
		return 0;
	}
	return (int64_t)(abs_ns - now);
}

static int
wait_bo_ioctl(void *arg)
{
	struct drm_panfrost_wait_bo *w = arg;
	int64_t timeout_ns;

	/*
	 * drm_panfrost_wait_bo.timeout_ns is documented as absolute but the
	 * only values panfrost's userspace passes are 0 (poll) and INT64_MAX
	 * ("forever"); the kernel's WAIT_BO is relative and caps -1 at 10 s.
	 */
	if ((uint64_t)w->timeout_ns == (uint64_t)INT64_MAX) {
		timeout_ns = SM_WAIT_INFINITE;
	} else if (w->timeout_ns < 0) {
		timeout_ns = SM_WAIT_INFINITE;
	} else {
		timeout_ns = w->timeout_ns;
	}
	return sm_wait_bo(w->handle, timeout_ns, NULL);
}

static int
sync_bo_ioctl(void *arg)
{
	struct drm_panfrost_sync_bo *s = arg;
	const struct drm_panfrost_bo_sync_op *ops =
	    (const struct drm_panfrost_bo_sync_op *)(uintptr_t)s->ops;
	uint32_t i;

	if (s->op_count == 0 || s->op_count > 64 || ops == NULL) {
		return -EINVAL;
	}
	for (i = 0; i < s->op_count; i++) {
		int ret = sm_sync_bo(ops[i].handle, ops[i].type,
		    ops[i].offset, ops[i].size);

		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

/*
 * `nr` is the low byte of the request in both the Linux and the Darwin
 * encoding of DRM_IOCTL_*, so this needs no #ifdef and no knowledge of which
 * libdrm the client was built against.  The sizes are ours, never the
 * request's, so a client built against a different libdrm version still gets
 * the struct this kernel expects.
 */
static int
sweet_ioctl(unsigned nr, void *arg)
{
	if (arg == NULL) {
		return -EINVAL;
	}
	switch (nr) {
	case DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT:
		return sm_submit(arg);

	case DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO:
		return wait_bo_ioctl(arg);

	case DRM_COMMAND_BASE + DRM_PANFROST_CREATE_BO: {
		struct drm_panfrost_create_bo *c = arg;
		uint32_t handle = 0;
		uint64_t gpu_va = 0;
		int ret;

		if (c->size == 0) {
			return -EINVAL;
		}
		/*
		 * The kernel's create_bo requires flags == 0 and takes `offset`
		 * in a different place than upstream's struct does; the
		 * panfrost BO flags (NOEXEC is set for every non-executable BO)
		 * have no meaning here -- the MMU mapping is made at create
		 * time.
		 */
		ret = sm_create_bo(c->size, &handle, NULL, &gpu_va);
		if (ret == 0) {
			c->handle = handle;
			c->pad = 0;
			/*
			 * `offset` in drm_panfrost_create_bo is the BO's GPU
			 * address, not the mmap file offset: Mesa's
			 * panfrost_kmod_bo_alloc() stores it as bo->offset and
			 * panfrost_bo_alloc() turns it into bo->ptr.gpu, then
			 * builds the job chain address and every descriptor
			 * pointer out of it.  MMAP_BO is the call that carries
			 * the file offset, and it answers the handle.
			 */
			c->offset = gpu_va;
		}
		return ret;
	}

	case DRM_COMMAND_BASE + DRM_PANFROST_MMAP_BO: {
		struct drm_panfrost_mmap_bo *m = arg;

		return sm_mmap_bo(m->handle, &m->offset);
	}

	case DRM_COMMAND_BASE + DRM_PANFROST_GET_PARAM: {
		struct drm_panfrost_get_param *p = arg;
		uint64_t value = 0;
		int ret;

		ret = sm_get_param(p->param, &value);
		if (ret == 0) {
			p->value = value;
		}
		return ret;
	}

	case DRM_COMMAND_BASE + DRM_PANFROST_GET_BO_OFFSET: {
		struct drm_panfrost_get_bo_offset *g = arg;

		return sm_get_bo_offset(g->handle, &g->offset);
	}

	case DRM_COMMAND_BASE + DRM_PANFROST_SYNC_BO:
		return sync_bo_ioctl(arg);

	case DRM_COMMAND_BASE + DRM_PANFROST_MADVISE: {
		struct drm_panfrost_madvise *m = arg;

		/*
		 * No swap-backed shrinking in this driver: the pages are always
		 * there, so WILLNEED retains and DONTNEED is a hint we cannot
		 * honour but can answer honestly (nothing is discarded).
		 */
		(void)m;
		m->retained = m->madv == PANFROST_MADV_WILLNEED ? 1 : 0;
		return 0;
	}

	case DRM_COMMAND_BASE + DRM_PANFROST_SET_LABEL_BO:
		/* A debugging label; the kernel keeps none. */
		return 0;

	case DRM_COMMAND_BASE + DRM_PANFROST_JM_CTX_CREATE:
	case DRM_COMMAND_BASE + DRM_PANFROST_JM_CTX_DESTROY:
		/* No scheduler, so no context priorities: panfrost only asks
		 * for one for HIGH/LOW priority pipe contexts. */
		return -ENOSYS;

	case DRM_COMMAND_BASE + DRM_PANFROST_PERFCNT_ENABLE:
	case DRM_COMMAND_BASE + DRM_PANFROST_PERFCNT_DUMP:
	case DRM_COMMAND_BASE + DRM_PANFROST_QUERY_BO_INFO:
		return -ENOSYS;

	case DRM_GEM_CLOSE: {
		struct drm_gem_close *c = arg;

		return sm_close_bo(c->handle);
	}

	default:
		return -ENOTTY;
	}
}

/* ------------------------------------------------------------------ */
/* libdrm API                                                          */
/* ------------------------------------------------------------------ */

/*
 * F3: the names SWEET_DRM_TRACE prints.  `nr` is the low byte of the request,
 * which for the panfrost family is DRM_COMMAND_BASE + the ioctl number (the
 * same arithmetic sweet_ioctl() switches on); DRM_GEM_CLOSE is its own low
 * byte and collides with none of them.
 */
static const char *
drm_ioctl_name(unsigned nr)
{
	switch (nr) {
	case DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT:
		return "PANFROST_SUBMIT";
	case DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO:
		return "PANFROST_WAIT_BO";
	case DRM_COMMAND_BASE + DRM_PANFROST_CREATE_BO:
		return "PANFROST_CREATE_BO";
	case DRM_COMMAND_BASE + DRM_PANFROST_MMAP_BO:
		return "PANFROST_MMAP_BO";
	case DRM_COMMAND_BASE + DRM_PANFROST_GET_PARAM:
		return "PANFROST_GET_PARAM";
	case DRM_COMMAND_BASE + DRM_PANFROST_GET_BO_OFFSET:
		return "PANFROST_GET_BO_OFFSET";
	case DRM_COMMAND_BASE + DRM_PANFROST_PERFCNT_ENABLE:
		return "PANFROST_PERFCNT_ENABLE";
	case DRM_COMMAND_BASE + DRM_PANFROST_PERFCNT_DUMP:
		return "PANFROST_PERFCNT_DUMP";
	case DRM_COMMAND_BASE + DRM_PANFROST_MADVISE:
		return "PANFROST_MADVISE";
	case DRM_COMMAND_BASE + DRM_PANFROST_SET_LABEL_BO:
		return "PANFROST_SET_LABEL_BO";
	case DRM_COMMAND_BASE + DRM_PANFROST_JM_CTX_CREATE:
		return "PANFROST_JM_CTX_CREATE";
	case DRM_COMMAND_BASE + DRM_PANFROST_JM_CTX_DESTROY:
		return "PANFROST_JM_CTX_DESTROY";
	case DRM_COMMAND_BASE + DRM_PANFROST_SYNC_BO:
		return "PANFROST_SYNC_BO";
	case DRM_COMMAND_BASE + DRM_PANFROST_QUERY_BO_INFO:
		return "PANFROST_QUERY_BO_INFO";
	case DRM_GEM_CLOSE:
		return "GEM_CLOSE";
	default:
		return "?";
	}
}

/*
 * F3 diagnostics: everything the client knows about a SUBMIT the kernel
 * refused.  The job header is the only witness -- the hardware overwrites its
 * first words with an exception record before it stops:
 *
 *   word 0   bits 0-7   JS_STATUS code (0x58 is DATA_INVALID_FAULT)
 *            bits 8-9   access type
 *   word 1               first incomplete task
 *   words 2-3            the pointer to the structure it rejected
 *
 * Resolving that pointer against the BO table is the point: it turns "invalid
 * data" into a named buffer and offset.
 */
static void
diag_dump_submit_fault(const struct drm_panfrost_submit *s)
{
	const uint32_t *handles = (const uint32_t *)(uintptr_t)s->bo_handles;
	uint64_t off = 0;
	uint32_t n;
	int idx;
	unsigned i;

	fprintf(stderr, "F3DIAG: SUBMIT refused jc=%#llx reqs=%#x bo_count=%u "
	    "out_sync=%u in_sync_count=%u\n", (unsigned long long)s->jc,
	    s->requirements, s->bo_handle_count, s->out_sync, s->in_sync_count);
	for (i = 0; handles != NULL && i < s->bo_handle_count &&
	    i < SM_SUBMIT_MAX_BOS; i++) {
		unsigned k;

		for (k = 0; k < SM_DIAG_BO_MAX; k++) {
			if (g_diag_bos[k].handle != handles[i]) {
				continue;
			}
			fprintf(stderr, "F3DIAG:   bo[%u] h=%u va=%#llx size=%u "
			    "cpu=%p%s\n", i, handles[i],
			    (unsigned long long)g_diag_bos[k].gpu_va,
			    g_diag_bos[k].size, g_diag_bos[k].cpu,
			    (s->jc >= g_diag_bos[k].gpu_va &&
			     s->jc < g_diag_bos[k].gpu_va +
			     g_diag_bos[k].size) ? " <- jc" : "");
			break;
		}
	}

	idx = diag_bo_find_va(s->jc, &off);
	if (idx < 0) {
		fprintf(stderr, "F3DIAG: jc is in no known BO\n");
	} else if (g_diag_bos[idx].cpu == NULL) {
		fprintf(stderr, "F3DIAG: jc BO h=%u (va=%#llx) has no CPU "
		    "mapping\n", g_diag_bos[idx].handle,
		    (unsigned long long)g_diag_bos[idx].gpu_va);
	} else {
		const volatile uint32_t *w = (const volatile uint32_t *)
		    ((const volatile char *)g_diag_bos[idx].cpu + off);
		uint32_t w0 = w[0];
		uint64_t fp = ((uint64_t)w[3] << 32) | w[2];
		uint64_t fo = 0;
		int fi;

		fprintf(stderr, "F3DIAG: job header h=%u + %#llx:\n",
		    g_diag_bos[idx].handle, (unsigned long long)off);
		fprintf(stderr, "F3DIAG:   ex=%#010x js_status=%#x access=%u "
		    "fic=%#010x fault_ptr=%#llx\n", w0, w0 & 0xffu,
		    (w0 >> 8) & 3u, w[1], (unsigned long long)fp);
		for (n = 0; n < 16; n++) {
			fprintf(stderr, "F3DIAG:   w[%2u]=%#010x\n", n, w[n]);
		}
		fi = diag_bo_find_va(fp, &fo);
		if (fp == 0) {
			fprintf(stderr, "F3DIAG:   fault_ptr is null\n");
		} else if (fi >= 0) {
			fprintf(stderr, "F3DIAG:   fault_ptr -> h=%u + %#llx "
			    "(size %u)\n", g_diag_bos[fi].handle,
			    (unsigned long long)fo, g_diag_bos[fi].size);
			if (g_diag_bos[fi].cpu != NULL && fo + 64 <=
			    g_diag_bos[fi].size) {
				const volatile uint32_t *f =
				    (const volatile uint32_t *)
				    ((const volatile char *)
				    g_diag_bos[fi].cpu + fo);

				for (n = 0; n < 16; n++) {
					fprintf(stderr, "F3DIAG:     fp[%2u]="
					    "%#010x\n", n, f[n]);
				}
			}
		} else {
			fprintf(stderr, "F3DIAG:   fault_ptr %#llx is in no "
			    "known BO\n", (unsigned long long)fp);
		}
	}

	/*
	 * panfrost's tiler heap is by far the largest BO; a tiler job reads
	 * its first words first, so they are the second-best witness.
	 */
	{
		int big = -1;
		unsigned k;

		for (k = 0; k < SM_DIAG_BO_MAX; k++) {
			if (g_diag_bos[k].handle != 0 &&
			    (big < 0 || g_diag_bos[k].size >
			    g_diag_bos[big].size)) {
				big = (int)k;
			}
		}
		if (big >= 0 && g_diag_bos[big].size >= 1024u * 1024u) {
			fprintf(stderr, "F3DIAG: largest BO h=%u va=%#llx "
			    "size=%u cpu=%p\n", g_diag_bos[big].handle,
			    (unsigned long long)g_diag_bos[big].gpu_va,
			    g_diag_bos[big].size, g_diag_bos[big].cpu);
			if (g_diag_bos[big].cpu != NULL) {
				const volatile uint32_t *h =
				    (const volatile uint32_t *)
				    g_diag_bos[big].cpu;

				for (n = 0; n < 8; n++) {
					fprintf(stderr, "F3DIAG:   heap w[%u]="
					    "%#010x\n", n, h[n]);
				}
			}
		}
	}
	fflush(stderr);
}

EXPORT int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned nr = (unsigned)(request & 0xffu);
	unsigned grp = (unsigned)((request >> 8) & 0xffu);
	int ret;

	if (!is_ours(fd)) {
		errno = ENOTTY;
		return -1;
	}
	if (grp != (unsigned)DRM_IOCTL_BASE) {
		errno = ENOTTY;
		return -1;
	}
	ret = sweet_ioctl(nr, arg);
	/*
	 * F3: SWEET_DRM_TRACE=1 makes every ioctl visible in the kernel ring,
	 * which is the only channel that survives to the host.  When Mesa
	 * stops at a NULL context that is the whole diagnosis: the failing
	 * translator is the one whose rc is not 0.
	 */
	if (g_trace) {
		if (nr == (unsigned)(DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT) &&
		    arg != NULL) {
			const struct drm_panfrost_submit *s = arg;

			/*
			 * F3: the count is the field that decides whether the
			 * kernel can take the job at all, and it is the one
			 * thing the errno cannot tell (EINVAL means "too many
			 * buffers" here as much as it means any core refusal).
			 */
			sweet_drm_log("ioctl nr=%#x (%s) rc=%d errno=%d "
			    "bo_count=%u jc=%#llx reqs=%#x out_sync=%u "
			    "in_sync_count=%u", nr, drm_ioctl_name(nr), ret,
			    ret != 0 ? errno_value(ret) : 0, s->bo_handle_count,
			    (unsigned long long)s->jc, s->requirements,
			    s->out_sync, s->in_sync_count);
			if (ret != 0) {
				diag_dump_submit_fault(s);
			}
		} else {
			sweet_drm_log("ioctl nr=%#x (%s) rc=%d errno=%d",
			    nr, drm_ioctl_name(nr), ret, ret != 0 ? errno_value(ret) : 0);
		}
	}
	if (ret != 0) {
		/*
		 * F3 diagnostics: pan_jm.c skips its trace/decode block when
		 * the SUBMIT ioctl fails, which is exactly when the decode is
		 * wanted.  Answering success lets Mesa run pandecode_jc() over
		 * the chain it built and name the structure the hardware
		 * rejected.  SWEET_DRM_IGNORE_FAULT is only set by the F3
		 * diagnostic run; the pixel check still fails, so nothing
		 * else is fooled.
		 */
		if (g_ignore_fault && arg != NULL &&
		    nr == (unsigned)(DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT)) {
			return 0;
		}
		errno = errno_value(ret);
		return -1;
	}
	return 0;
}

EXPORT int
drmOpen(const char *name, const char *busid)
{
	return drmOpenWithType(name, busid, DRM_NODE_PRIMARY);
}

EXPORT int
drmOpenWithType(const char *name, const char *busid, int type)
{
	int fd;

	(void)busid;
	(void)type;
	if (name != NULL && strcmp(name, SWEET_DRM_VERSION_NAME) != 0) {
		errno = ENODEV;
		return -1;
	}
	if (!connect_once()) {
		errno = ENODEV;
		return -1;
	}
	fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		errno = ENODEV;
		return -1;
	}
	return fd;
}

EXPORT int
sweet_drm_open(void)
{
	return drmOpenWithType(SWEET_DRM_VERSION_NAME, NULL, DRM_NODE_RENDER);
}

EXPORT int
drmClose(int fd)
{
	return close(fd);
}

EXPORT int
drmAvailable(void)
{
	return g_conn != IO_OBJECT_NULL || connect_once();
}

EXPORT int
drmGetNodeTypeFromFd(int fd)
{
	return is_ours(fd) ? DRM_NODE_RENDER : -1;
}

EXPORT drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr v;

	if (!is_ours(fd)) {
		errno = ENODEV;
		return NULL;
	}
	v = calloc(1, sizeof(*v));
	if (v == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	v->version_major = SWEET_DRM_VERSION_MAJOR;
	v->version_minor = SWEET_DRM_VERSION_MINOR;
	v->version_patchlevel = 0;
	v->name = strdup(SWEET_DRM_VERSION_NAME);
	v->name_len = (int)strlen(SWEET_DRM_VERSION_NAME);
	v->date = strdup("");
	v->date_len = 0;
	v->desc = strdup(SWEET_DRM_VERSION_DESC);
	v->desc_len = (int)strlen(SWEET_DRM_VERSION_DESC);
	if (v->name == NULL || v->date == NULL || v->desc == NULL) {
		drmFreeVersion(v);
		errno = ENOMEM;
		return NULL;
	}
	return v;
}

EXPORT void
drmFreeVersion(drmVersionPtr v)
{
	if (v == NULL) {
		return;
	}
	free(v->name);
	free(v->date);
	free(v->desc);
	free(v);
}

EXPORT int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	if (!is_ours(fd)) {
		errno = ENODEV;
		return -1;
	}
	if (value == NULL) {
		errno = EINVAL;
		return -1;
	}
	switch (capability) {
	case DRM_CAP_DUMB_BUFFER:
		*value = 0;
		return 0;
	case DRM_CAP_PRIME:
		/* No dma-buf: see drmPrime* below. */
		*value = 0;
		return 0;
	case DRM_CAP_SYNCOBJ:
		/* Real sync objects, but no sync_files: DRM_CAP_SYNCOBJ is
		 * about the syncobj ioctls, which we do implement. */
		*value = 1;
		return 0;
	case DRM_CAP_SYNCOBJ_TIMELINE:
		*value = 0;
		return 0;
	default:
		errno = EINVAL;
		return -1;
	}
}

EXPORT int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close c = {
		.handle = handle,
		.pad = 0,
	};

	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

EXPORT int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	errno = ENOSYS;
	return -1;
}

EXPORT int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd;
	(void)prime_fd;
	(void)handle;
	errno = ENOSYS;
	return -1;
}

/*
 * Device enumeration: there is no /dev/dri, and nothing in panfrost's submit
 * path needs a drmDevice, so this answers loudly rather than handing out a
 * struct that a caller would read as a PCI device.
 */
EXPORT int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	(void)device;
	errno = ENOSYS;
	return -1;
}

EXPORT void
drmFreeDevice(drmDevicePtr *device)
{
	(void)device;
}

EXPORT int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	(void)devices;
	(void)max_devices;
	errno = ENOSYS;
	return -1;
}

EXPORT void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	(void)devices;
	(void)count;
}

/* ---- sync objects ---- */

EXPORT int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	int ret;

	if (!is_ours(fd) || handle == NULL) {
		errno = ENODEV;
		return -1;
	}
	if ((flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) != 0) {
		errno = EINVAL;
		return -1;
	}
	/*
	 * DRM_SYNCOBJ_CREATE_SIGNALED is passed through: the kernel has a
	 * SM_SYNCOBJ_CREATE_SIGNALED for exactly this, because Mesa's
	 * panfrost refuses to build a context without a fence it can create
	 * already signalled (pan_context.c: the out_sync of the last queued
	 * job, which is "no job" until the first submit).
	 */
	ret = sm_syncobj_create(flags, handle);
	if (ret != 0) {
		errno = errno_value(ret);
		return -1;
	}
	return 0;
}

EXPORT int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	int ret;

	if (!is_ours(fd)) {
		errno = ENODEV;
		return -1;
	}
	ret = sm_syncobj_destroy(handle);
	if (ret != 0) {
		errno = errno_value(ret);
		return -1;
	}
	return 0;
}

EXPORT int
drmSyncobjWait(int fd, const uint32_t *handles, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	unsigned i;

	if (!is_ours(fd) || handles == NULL || num_handles == 0) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < num_handles; i++) {
		uint32_t signaled = 0;
		int64_t rel;
		int ret;

		rel = timeout_nsec < 0 ? SM_WAIT_INFINITE :
		    abs_to_rel_ns((uint64_t)timeout_nsec);
		ret = sm_syncobj_wait(handles[i], rel, &signaled);
		if (ret != 0) {
			errno = errno_value(ret);
			return -1;
		}
		if (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) {
			if (first_signaled != NULL && i == 0) {
				*first_signaled = handles[i];
			}
			continue;
		}
		if (first_signaled != NULL) {
			*first_signaled = handles[i];
		}
		return 0;
	}
	return 0;
}

EXPORT int
drmSyncobjTimelineWait(int fd, const uint32_t *handles, uint64_t *points,
    unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	(void)fd;
	(void)handles;
	(void)points;
	(void)num_handles;
	(void)timeout_nsec;
	(void)flags;
	(void)first_signaled;
	errno = ENOSYS;
	return -1;
}

EXPORT int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd;
	(void)handle;
	(void)obj_fd;
	errno = ENOSYS;
	return -1;
}

EXPORT int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd;
	(void)obj_fd;
	(void)handle;
	errno = ENOSYS;
	return -1;
}

EXPORT int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	(void)handle;
	(void)sync_file_fd;
	errno = ENOSYS;
	return -1;
}

EXPORT int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd;
	(void)handle;
	(void)sync_file_fd;
	errno = ENOSYS;
	return -1;
}

EXPORT int
drmSyncobjQuery(int fd, const uint32_t *handles, uint64_t *points,
    uint32_t handle_count)
{
	return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

EXPORT int
drmSyncobjQuery2(int fd, const uint32_t *handles, uint64_t *points,
    uint32_t handle_count, uint32_t flags)
{
	(void)fd;
	(void)handles;
	(void)points;
	(void)handle_count;
	(void)flags;
	errno = ENOSYS;
	return -1;
}

EXPORT int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	(void)handles;
	(void)handle_count;
	errno = ENOSYS;
	return -1;
}

EXPORT int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	(void)handles;
	(void)handle_count;
	errno = ENOSYS;
	return -1;
}

EXPORT int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
    uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
	(void)fd;
	(void)dst_handle;
	(void)dst_point;
	(void)src_handle;
	(void)src_point;
	(void)flags;
	errno = ENOSYS;
	return -1;
}

/* ---- mapping a BO ---- */

static struct {
	void *addr;
	uint32_t handle;
	size_t len;
} g_maps[16384];
static unsigned g_nmaps;

static void
map_record(void *addr, uint32_t handle, size_t len)
{
	pthread_mutex_lock(&g_lock);
	if (g_nmaps < sizeof(g_maps) / sizeof(g_maps[0])) {
		g_maps[g_nmaps].addr = addr;
		g_maps[g_nmaps].handle = handle;
		g_maps[g_nmaps].len = len;
		g_nmaps++;
	}
	pthread_mutex_unlock(&g_lock);
}

static int
map_forget(void *addr, uint32_t *handle)
{
	unsigned i;
	int found = 0;

	pthread_mutex_lock(&g_lock);
	for (i = 0; i < g_nmaps; i++) {
		if (g_maps[i].addr == addr) {
			if (handle != NULL) {
				*handle = g_maps[i].handle;
			}
			g_maps[i] = g_maps[g_nmaps - 1];
			g_nmaps--;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&g_lock);
	return found;
}

static void *
sweet_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
static int sweet_munmap(void *addr, size_t len);

EXPORT void *
sweet_drm_mmap(int fd, size_t len, uint64_t offset)
{
	return sweet_mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    (off_t)offset);
}

EXPORT int
sweet_drm_munmap(void *addr, size_t len)
{
	return sweet_munmap(addr, len);
}

static void *
sweet_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	mach_vm_address_t a = 0;
	mach_vm_size_t size = 0;
	kern_return_t kr;
	int ours = is_ours(fd);

	if (!ours) {
		/*
		 * Not one of our fds.  A nonzero offset on someone else's fd is
		 * worth a line: it is how a missed interpose shows up (the real
		 * mmap then rejects the unaligned BO offset with EINVAL).
		 */
		if (off != 0) {
			sweet_drm_log("mmap? fd=%d off=%lld len=%zu (not ours)",
			    fd, (long long)off, len);
		}
		return mmap(addr, len, prot, flags, fd, off);
	}
	sweet_drm_log("mmap(ours) off=%lld len=%zu prot=%d flags=%d",
	    (long long)off, len, prot, flags);
	if (len == 0) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	/*
	 * The offset MMAP_BO returned is the BO handle, which is what
	 * clientMemoryForType() gets as its `type`.
	 */
	kr = IOConnectMapMemory64(g_conn, (uint32_t)off, mach_task_self(), &a,
	    &size, kIOMapAnywhere);
	if (kr != KERN_SUCCESS || size < len) {
		sweet_drm_log("mmap FAILED off=%lld kr=0x%x size=%llu len=%zu",
		    (long long)off, (unsigned)kr, (unsigned long long)size, len);
		if (kr == KERN_SUCCESS) {
			IOConnectUnmapMemory64(g_conn, (uint32_t)off,
			    mach_task_self(), a);
			errno = EINVAL;
		} else {
			errno = io_errno(kr);
		}
		return MAP_FAILED;
	}
	sweet_drm_log("mmap ok off=%lld at 0x%llx size=%llu",
	    (long long)off, (unsigned long long)a, (unsigned long long)size);
	map_record((void *)(uintptr_t)a, (uint32_t)off, len);
	diag_bo_note_mmap((uint32_t)off, (void *)(uintptr_t)a);
	return (void *)(uintptr_t)a;
}

static int
sweet_munmap(void *addr, size_t len)
{
	uint32_t handle = 0;
	int ours;

	ours = map_forget(addr, &handle);
	if (!ours) {
		return munmap(addr, len);
	}
	return io_errno(IOConnectUnmapMemory64(g_conn, handle,
	    mach_task_self(), (mach_vm_address_t)(uintptr_t)addr)) == 0 ? 0 : -1;
}

struct interpose_tuple {
	const void *replacement;
	const void *replacee;
};

__attribute__((used)) static const struct interpose_tuple interpose_mmap
    __attribute__((section("__DATA,__interpose"))) = {
	(const void *)sweet_mmap, (const void *)mmap
};
__attribute__((used)) static const struct interpose_tuple interpose_munmap
    __attribute__((section("__DATA,__interpose"))) = {
	(const void *)sweet_munmap, (const void *)munmap
};
