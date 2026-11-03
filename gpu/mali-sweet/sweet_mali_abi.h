/*
 * SweetMali ABI: what a client process may ask the Mali-G57 kernel service for.
 *
 * This header is deliberately free of kernel types -- no `sys/`, no IOKit --
 * so that the same file compiles in the kernel (the user client's handlers)
 * and in userspace (the libdrm stand-in that D/F builds over the IOKit
 * connection, modelled on gpu/freedreno-sweet/sweet_drm.c in the bergamota
 * tree).  When that stand-in lands, this file is copied into the userspace
 * tree; until then the kernel copy under bsd/sys is the only one, and the
 * client program includes it by path.
 *
 * The shape follows bergamota's xnu/gpu-abi/sweet_adreno_abi.h, which is
 * proven on hardware: selectors are small integers, arguments travel as a
 * struct through IOConnectCallStructMethod, and the selector number for the
 * GPU calls is the panfrost ioctl number they stand in for, so the userspace
 * marshaller stays a table lookup rather than a translation layer.
 *
 * A note on `struct sm_get_param`: the fields are ordered to match the DRM
 * struct it stands in for (struct drm_panfrost_get_param), i.e. two 32-bit
 * fields and then a 64-bit value, which on arm64 means an 8-byte alignment
 * hole after `param` and before `value`.  That hole is spelled out as `pad`
 * rather than left to the compiler so the kernel and the client cannot
 * disagree about the layout.
 */
#ifndef _BSD_DEV_SWEET_MALI_ABI_H_
#define _BSD_DEV_SWEET_MALI_ABI_H_

#ifndef SM_ABI_STANDALONE
#include <stdint.h>
#else
#include <stdint.h>
#endif

/*
 * Bumped when a struct below changes meaning; GET_PARAM VERSION returns it.
 * 2: D3 extends struct sm_submit (out_sync is an input fence handle now,
 *    in_syncs[] was added) and adds the fence/wait/sync structs.
 * 3: F3 changes layouts D2/D3 clients did share, so the kernel and its client
 *    must be built together (they ship in one image):
 *      - sm_syncobj_create's `pad` is `flags` (SM_SYNCOBJ_CREATE_SIGNALED);
 *      - SM_SUBMIT_MAX_BOS grows 8 -> 32, which grows struct sm_submit's
 *        inline arrays (one panfrost job names every BO of the batch);
 *      - sm_create_bo grows a `gpu_va` answer, the BO's address in the GPU
 *        address space, without which no job chain address means anything.
 * 4: F3 takes the job slot out of the kernel: struct sm_submit's `pad` is now
 *    `requirements`, the DRM PANFROST_JD_REQ_* word, because the slot is not
 *    the kernel's to guess.  JS0 is the fragment slot and JS1 the
 *    vertex/tiler one -- upstream panfrost (PANFROST_JD_REQ_FS -> JS0,
 *    anything else -> JS1) and MediaTek's kbase agree on that split, and this
 *    kernel's own panel demo already submits its IDVS job on JS1 and its
 *    fragment job on JS0.  A vertex/tiler job started on JS0 is refused by
 *    the hardware with DATA_INVALID_FAULT (JS_STATUS 0x58), which is what
 *    every panfrost job chain hit while the slot was hard-coded.  The
 *    kernel's own user-client self-test says which slot its jobs want too:
 *    they are the panel demo's fragment-class write-value job, so it asks for
 *    JS0, and its "the job completed" checks keep reading JS0's bit.
 * 5: E adds SM_SEL_SURFACE_BO and struct sm_surface_bo: an IOSurface's pages
 *    as a BO, so a render target can be the surface the compositor is going
 *    to scan out.  Nothing existing changes: a client that does not call the
 *    new selector cannot tell the version moved.
 * F1/B2 added only selectors and new structs; no existing layout changed
 * there, which is why it did not move this number.
 */
#define SM_ABI_VERSION          5u

/*
 * Selectors.  The first block mirrors DRM_IOCTL_PANFROST_*; the second is
 * SweetMali's own (there is no DRM ioctl behind it, the port has no DRM).
 */
#define SM_SEL_VERSION          0x00u   /* out: struct sm_abi_info */
#define SM_SEL_GET_PARAM        0x01u   /* in/out: struct sm_get_param */
/* Added by stage D2: client-owned GPU memory and job submission. */
#define SM_SEL_CREATE_BO        0x02u   /* in/out: struct sm_create_bo */
#define SM_SEL_GET_BO_OFFSET    0x03u   /* in/out: struct sm_get_bo_offset */
#define SM_SEL_CLOSE_BO         0x04u   /* in: struct sm_close_bo */
#define SM_SEL_SUBMIT           0x05u   /* in/out: struct sm_submit */
/* Added by stage D3: fences, waits, cache sync. */
#define SM_SEL_WAIT_BO          0x06u   /* in/out: struct sm_wait_bo */
#define SM_SEL_SYNC_BO          0x07u   /* in: struct sm_sync_bo */
#define SM_SEL_SYNCOBJ_CREATE   0x08u   /* in/out: struct sm_syncobj_create */
#define SM_SEL_SYNCOBJ_DESTROY  0x09u   /* in: struct sm_syncobj_destroy */
#define SM_SEL_SYNCOBJ_WAIT     0x0au   /* in/out: struct sm_syncobj_wait */
/* Added by stage F1/B2: client mapping and a kernel-log readback channel. */
#define SM_SEL_MMAP_BO          0x0bu   /* in/out: struct sm_mmap_bo */
#define SM_SEL_LOG              0x0cu   /* in/out: struct sm_log */
/* Added by stage E: an IOSurface's pages as a BO the GPU renders into. */
#define SM_SEL_SURFACE_BO       0x0du   /* in/out: struct sm_surface_bo */

/*
 * F3: the create flags, in the ABI's own spelling.  Mesa's panfrost opens its
 * context with a sync object that is *already signalled* (pan_context.c: "the
 * last queued job out_sync") and refuses a context if it cannot have one, so a
 * client has to be able to ask for it.  It is honest here: no job has taken
 * the object yet, and every SUBMIT that does take it signals it again when the
 * job finishes.
 */
#define SM_SYNCOBJ_CREATE_SIGNALED 0x1u

/*
 * F3: the one submission requirement this port implements, spelled exactly as
 * the DRM ABI spells it (PANFROST_JD_REQ_FS) because the client hands its word
 * straight through.  Setting it asks for the fragment slot; a submission that
 * does not set it is a vertex/tiler job and takes the other one.  Any other
 * bit is refused rather than ignored: cycle counting
 * (PANFROST_JD_REQ_CYCLE_COUNT) is not implemented here, and a client that
 * asks for it deserves an answer, not a silently missing counter.
 */
#define SM_SUBMIT_REQ_FS        0x1u
#define SM_SUBMIT_REQ_ALL       0x1u

/* The service's class name in the IORegistry, and its user client class. */
#define SM_SERVICE_NAME         "SweetMali"
#define SM_USER_CLIENT_CLASS    "SweetMaliUserClient"

struct sm_abi_info {
	uint32_t abi_version;   /* SM_ABI_VERSION */
	uint32_t ready;         /* 1 = bring-up finished, GPU powered; 0 = not */
};

/*
 * Param ids: the order is `include/drm-uapi/panfrost_drm.h`'s unnamed enum
 * (DRM_PANFROST_PARAM_GPU_PROD_ID = 0, ...), so a client that knows upstream's
 * numbering does not need a translation table.  Ids the driver cannot answer
 * return kIOReturnUnsupported rather than a wrong number -- an absent feature
 * and a feature that reads as zero are different facts, and a client that
 * cannot tell them apart will mis-detect the GPU.
 */
#define SM_PARAM_GPU_PROD_ID                0u
#define SM_PARAM_GPU_REVISION               1u
#define SM_PARAM_SHADER_PRESENT             2u
#define SM_PARAM_TILER_PRESENT              3u
#define SM_PARAM_L2_PRESENT                 4u
#define SM_PARAM_STACK_PRESENT              5u
#define SM_PARAM_AS_PRESENT                 6u
#define SM_PARAM_JS_PRESENT                 7u
#define SM_PARAM_L2_FEATURES                8u
#define SM_PARAM_CORE_FEATURES              9u
#define SM_PARAM_TILER_FEATURES             10u
#define SM_PARAM_MEM_FEATURES               11u
#define SM_PARAM_MMU_FEATURES               12u
#define SM_PARAM_THREAD_FEATURES            13u
#define SM_PARAM_MAX_THREADS                14u
#define SM_PARAM_THREAD_MAX_WORKGROUP_SZ    15u
#define SM_PARAM_THREAD_MAX_BARRIER_SZ      16u
#define SM_PARAM_COHERENCY_FEATURES         17u
#define SM_PARAM_TEXTURE_FEATURES0          18u
#define SM_PARAM_TEXTURE_FEATURES1          19u
#define SM_PARAM_TEXTURE_FEATURES2          20u
#define SM_PARAM_TEXTURE_FEATURES3          21u
#define SM_PARAM_JS_FEATURES0               22u
#define SM_PARAM_JS_FEATURES1               23u
#define SM_PARAM_JS_FEATURES2               24u
#define SM_PARAM_JS_FEATURES3               25u
#define SM_PARAM_JS_FEATURES4               26u
#define SM_PARAM_JS_FEATURES5               27u
#define SM_PARAM_JS_FEATURES6               28u
#define SM_PARAM_JS_FEATURES7               29u
#define SM_PARAM_JS_FEATURES8               30u
#define SM_PARAM_JS_FEATURES9               31u
#define SM_PARAM_JS_FEATURES10              32u
#define SM_PARAM_JS_FEATURES11              33u
#define SM_PARAM_JS_FEATURES12              34u
#define SM_PARAM_JS_FEATURES13              35u
#define SM_PARAM_JS_FEATURES14              36u
#define SM_PARAM_JS_FEATURES15              37u
#define SM_PARAM_NR_CORE_GROUPS             38u
#define SM_PARAM_THREAD_TLS_ALLOC           39u
#define SM_PARAM_AFBC_FEATURES              40u
#define SM_PARAM_SYSTEM_TIMESTAMP           41u
#define SM_PARAM_SYSTEM_TIMESTAMP_FREQUENCY 42u
/* SM_PARAM_ALLOWED_JM_CTX_PRIORITIES 43u -- no scheduler yet, unsupported */
/* SM_PARAM_SELECTED_COHERENCY         44u -- no coherency protocol yet      */
#define SM_PARAM_COUNT                      45u

struct sm_get_param {
	uint32_t param;         /* one of SM_PARAM_* */
	uint32_t pad;           /* layout, not information */
	uint64_t value;         /* out */
};

/*
 * D2: memory and submission; D3: fences, waits and cache sync.
 *
 * These are the panfrost UAPI calls a first userspace needs:
 *
 *   SM_SEL_CREATE_BO      <- DRM_IOCTL_PANFROST_CREATE_BO
 *   SM_SEL_GET_BO_OFFSET  <- DRM_IOCTL_PANFROST_GET_BO_OFFSET
 *   SM_SEL_MMAP_BO        <- DRM_IOCTL_PANFROST_MMAP_BO      (F1/B2)
 *   SM_SEL_SUBMIT         <- DRM_IOCTL_PANFROST_SUBMIT
 *   SM_SEL_WAIT_BO        <- DRM_IOCTL_PANFROST_WAIT_BO
 *   SM_SEL_SYNC_BO        <- DRM_IOCTL_PANFROST_SYNC_BO
 *   SM_SEL_CLOSE_BO       <- DRM_IOCTL_GEM_CLOSE (generic DRM, not panfrost)
 *
 * CREATE_BO hands back a handle and an mmap offset.  GET_BO_OFFSET answers
 * the BO's GPU virtual address -- the number a job descriptor has to name --
 * exactly as upstream does, so the userspace half keeps one number per BO and
 * no VA arithmetic of its own.  `offset` in sm_create_bo is the *CPU* mapping
 * offset, not the GPU one; it equals the handle, and F1/B2's MMAP_BO returns
 * the same number after validating the handle.  The actual pages come from
 * IOConnectMapMemory64(), whose memory type is that offset.
 *
 * SUBMIT carries the BO and sync handles inline instead of through pointers
 * like drm_panfrost_submit, because our transport is a copied struct and not
 * an ioctl with copy_from_user.  The userspace marshaller fills the arrays in
 * the struct; the kernel never dereferences a user pointer.  A job that names
 * more than SM_SUBMIT_MAX_BOS buffers or SM_SUBMIT_MAX_SYNCS in-fences is
 * refused rather than truncated.
 *
 * D3 adds fences the way panfrost does, with one deliberate simplification:
 * this driver still runs the job to completion inside SUBMIT (the polled wait
 * the probe suite uses), so the out fence is signalled before SUBMIT returns
 * and an in fence must already be signalled for the job to start.  That is a
 * correct, if unambitious, implementation of the UAPI: a client that waits on
 * the out fence succeeds, and a chain of submits that names the previous out
 * fence as its in fence runs in order.  A real job queue (and therefore
 * overlapping CPU/GPU work) belongs with the Mesa winsys in F2; nothing in
 * this ABI changes when it arrives.
 *
 * Sync objects are SweetMali's own because the port has no DRM; the numbers
 * and semantics are DRM's syncobj create/destroy/wait, with the handle inline.
 * WAIT_BO and SYNC_BO keep panfrost's shapes (a BO plus a timeout; one BO and
 * one cache op), but their pointers are inline too.
 *
 * On arm64 the layout has no implicit holes: every 64-bit field is 8-byte
 * aligned and every gap is spelled `pad`, so the client and the kernel cannot
 * disagree about a struct because one compiler inserted padding.
 */
#define SM_BO_PAGE_SIZE         0x1000u
/*
 * F3: one panfrost job names *every* buffer the batch touches in a single
 * SUBMIT -- the batch's own resources (colour buffer, vertex/index buffers),
 * both BO pools it allocates from (descriptors and shaders), the tiler heap
 * and the sample-positions table -- so a limit of 8 turned every real draw
 * into SM_SUBMIT_EBADARG (the exact refusal seen as PANFROST_SUBMIT rc=22).
 * A job may name as many buffers as a client can hold at all (SM_BO_MAX, 32
 * below), which is why the two numbers agree; the arrays are inline in struct
 * sm_submit, so this is the one place the limit is spelled and both sides must
 * be rebuilt together when it moves.
 */
#define SM_SUBMIT_MAX_BOS       32u
#define SM_SUBMIT_MAX_SYNCS     8u
#define SM_SYNCOBJ_MAX          16u
/*
 * The handle table's size, and so the largest BO handle a client can hold.
 * It is ABI because a handle is also the memory type a client passes to
 * IOConnectMapMemory64(): the user client indexes its per-handle descriptor
 * cache with it, and a handle at or above it must never reach that cache.
 */
#define SM_BO_MAX               32u

/*
 * timeout_ns is relative: 0 polls, >0 waits that many nanoseconds, <0 means
 * "as long as this stage can": D3 caps an infinite wait at 10 s because a
 * synchronous submit has no other thread that could ever signal the fence.
 */
#define SM_WAIT_INFINITE        (-1ll)

/* sm_sync_bo.type: panfrost's PANFROST_BO_SYNC_* values. */
#define SM_BO_SYNC_CPU_CACHE_FLUSH                      0u
#define SM_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE       1u

struct sm_create_bo {
	uint32_t size;          /* in: bytes, a multiple of SM_BO_PAGE_SIZE */
	uint32_t flags;         /* in: reserved, must be 0 */
	uint64_t offset;        /* out: CPU mmap offset (D2: equals handle) */
	uint32_t handle;        /* out: BO handle for the calls below */
	uint32_t pad;
	/*
	 * F3: the BO's GPU address.  It is what a driver needs and what D2
	 * never had to publish: panfrost takes the address space base of every
	 * BO from exactly here (panfrost_kmod_bo_alloc(): `bo->offset =
	 * req.offset`), then expresses the job chain address, every descriptor
	 * pointer and the tiler heap address in that space.  A client left to
	 * invent its own addresses builds jobs whose `jc` means nothing to the
	 * MMU, which is refused before the job ever reaches the GPU.
	 */
	uint64_t gpu_va;        /* out: the BO's address in the GPU AS */
};

struct sm_get_bo_offset {
	uint32_t handle;        /* in */
	uint32_t pad;
	uint64_t offset;        /* out: GPU VA of the BO's first byte */
};

struct sm_close_bo {
	uint32_t handle;        /* in */
	uint32_t pad;
};

/*
 * D3: one sync object.  Creating one makes an unsignalled fence; a SUBMIT
 * whose out_sync names it signals it when the job finishes -- or faults, or
 * times out, because a fence that never signals is a hang and a failed job is
 * still a finished job.  SUBMIT's return is where the error is reported.
 *
 * Handles start at 1: 0 in a submit means "no fence", so it cannot also name
 * the first sync object.  This is the one place where SweetMali differs from
 * DRM, where 0 is a legal handle; the userspace marshaller adds the offset.
 */
struct sm_syncobj_create {
	uint32_t handle;        /* out */
	uint32_t flags;         /* in: SM_SYNCOBJ_CREATE_SIGNALED */
};

struct sm_syncobj_destroy {
	uint32_t handle;        /* in */
	uint32_t pad;
};

struct sm_syncobj_wait {
	uint32_t handle;        /* in */
	uint32_t pad;
	int64_t timeout_ns;     /* in: relative; see SM_WAIT_INFINITE */
	uint32_t signaled;      /* out: 1 = signalled, 0 = timed out */
	uint32_t pad2;
};

/*
 * WAIT_BO waits for the last SUBMIT that named the BO to finish.  In D3 the
 * submit is synchronous, so a BO is only busy while another thread is inside
 * SUBMIT; the call is still the shape panfrost's userspace expects.
 */
struct sm_wait_bo {
	uint32_t handle;        /* in */
	uint32_t pad;
	int64_t timeout_ns;     /* in: relative; see SM_WAIT_INFINITE */
	uint32_t signaled;      /* out: 1 = idle, 0 = timed out */
	uint32_t pad2;
};

/*
 * SYNC_BO performs cache maintenance on a range of a BO.  `size == 0` means
 * "to the end of the BO".  One op per call; panfrost's array of ops becomes
 * a loop in the userspace marshaller.
 */
struct sm_sync_bo {
	uint32_t handle;        /* in */
	uint32_t type;          /* in: SM_BO_SYNC_* */
	uint32_t offset;        /* in: byte offset, must be < BO size */
	uint32_t size;          /* in: bytes, 0 = to the end of the BO */
};

struct sm_submit {
	uint64_t jc;            /* in: GPU VA of a job descriptor in a BO */
	uint32_t bo_count;      /* in: 1..SM_SUBMIT_MAX_BOS */
	uint32_t requirements;  /* in: SM_SUBMIT_REQ_*, i.e. which job slot */
	uint32_t bo_handles[SM_SUBMIT_MAX_BOS]; /* in */
	uint32_t rawstat;       /* out: JOB_INT_RAWSTAT, for diagnosis */
	uint32_t pad2;
	uint64_t out_sync;      /* in: syncobj to signal, or 0 for none */
	uint32_t in_sync_count; /* in: 0..SM_SUBMIT_MAX_SYNCS */
	uint32_t in_syncs[SM_SUBMIT_MAX_SYNCS]; /* in: syncobjs to wait for */
};

/*
 * F1/B2: MMAP_BO is the last panfrost ioctl the kernel half was missing.
 * Upstream returns a byte offset for mmap(2); here the offset *is* the BO
 * handle, because IOConnectMapMemory64() takes the memory type (the handle)
 * and the user client's clientMemoryForType() turns it into a physical
 * descriptor.  A client never does arithmetic on the number; it passes it
 * straight to mmap (libdrm stand-in) or to IOConnectMapMemory64.
 */
struct sm_mmap_bo {
	uint32_t handle;        /* in */
	uint32_t pad;
	uint64_t offset;        /* out: CPU mmap offset, equal to handle */
};

/*
 * F1: a bounded text line for the bring-up log.  There is no usable file
 * readback path from the host (the root is HFS+ and Android cannot mount it),
 * so a userspace probe writes its verdict through the kernel's IOLog, which
 * lands in the console ring the recovery path already pulls.  The struct is
 * in/out so a client can use one buffer for the call; status is 1 when the
 * line was accepted.  This selector is diagnostic, not a Mesa interface.
 */
#define SM_LOG_MAX              240u
struct sm_log {
	uint32_t size;          /* in: bytes used, 1..SM_LOG_MAX */
	uint32_t status;        /* out: 1 = logged */
	char text[SM_LOG_MAX];  /* in: NUL not required */
};

/*
 * E: an IOSurface's pixels as a BO.
 *
 * The pages behind `surface_id` (what IOSurfaceGetID() returns) are entered
 * into the GPU address space, and the handle that comes back can be named by
 * SM_SEL_SUBMIT exactly like a BO's.  Nothing is copied: the GPU writes the
 * same physical pages the CPU sees through its own IOSurface mapping, which
 * is the point -- a render target is then the surface the compositor is going
 * to scan out, not a private buffer that has to be copied there afterwards.
 *
 * `surface_id` must name a surface the kernel knows AND that is backed by
 * kernel-owned memory.  A surface wrapping client memory is refused: those
 * pages are pageable and can move out from under the GPU.
 *
 * `handle` on input is SM_SURFACE_DESCRIBE_ONLY to ask only for the geometry
 * (nothing is mapped, `handle` comes back 0); otherwise it must be 0 and the
 * answer is a handle like CREATE_BO's.
 *
 * `offset` is where pixel (0, 0) sits inside the mapped range.  The mapping
 * starts at the surface's own memory base -- the handle's GPU address is byte
 * 0 of that range -- so a caller drawing at (x, y) writes to
 * `gpu_va + offset + y * bytes_per_row + x * pixelsize`.
 *
 * The pages keep the surface's cache attributes, so the CPU/GPU handover goes
 * through SM_SEL_SYNC_BO, as it does for any other BO.  Skipping it shows up
 * as stale or torn pixels, not as an error.
 *
 * Closing the handle (SM_SEL_CLOSE_BO) unmaps the pages from the GPU address
 * space and stops the kernel holding the surface; it never frees memory the
 * kernel did not allocate.
 */
#define SM_SURFACE_DESCRIBE_ONLY 0xffffffffu
struct sm_surface_bo {
	uint32_t surface_id;    /* in: IOSurfaceGetID() */
	uint32_t handle;        /* in: DESCRIBE_ONLY or 0; out: BO handle */
	uint64_t size;          /* out: bytes mapped (whole pages) */
	uint64_t gpu_va;        /* out: GPU address of the range's byte 0 */
	uint32_t offset;        /* out: byte offset of pixel (0, 0) in the range */
	uint32_t bytes_per_row; /* out */
	uint32_t width;         /* out */
	uint32_t height;        /* out */
	uint32_t pixel_format;  /* out: the surface's, not the GPU's */
	uint32_t planes;        /* out */
};

#endif /* _BSD_DEV_SWEET_MALI_ABI_H_ */
