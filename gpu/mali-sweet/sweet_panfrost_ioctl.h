/*
 * sweet_panfrost_ioctl.h: the slice of the panfrost DRM UAPI the libdrm
 * stand-in has to understand, vendored so this directory builds without Mesa
 * or libdrm on the include path.
 *
 * The structs and the ioctl numbers are copied from
 *   include/drm-uapi/panfrost_drm.h
 * (Mesa 27.0.0-devel, sha of the tree is in README.md) together with the
 * DRM_IOW/DRM_IOWR encoding from include/drm-uapi/drm.h.  They are vendored
 * rather than included because the iOS build has no libdrm, and because the
 * stand-in must agree with whatever libdrm the *client* (Mesa) was built
 * against -- which is the point of the nr-only decode documented in
 * sweet_drm.c.
 *
 * Why the encoding is copied verbatim matters: on Linux the DRM_IOWR macro
 * builds the request with <sys/ioctl.h>'s _IOWR, and on Darwin Mesa's drm.h
 * pulls the same macro name from <sys/ioccom.h>, which puts the direction bits
 * and the size at the same bit offsets but masks the size field differently.
 * For every request below the two encodings agree, and both keep the ioctl
 * number in the low byte and the type/pseudo-group in bits 8..15, so the
 * stand-in decodes `nr` and ignores the rest.  The constants here are still
 * useful: the test uses them as the request values it hands to drmIoctl().
 */
#ifndef _SWEET_PANFROST_IOCTL_H_
#define _SWEET_PANFROST_IOCTL_H_

#include <stdint.h>

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef int32_t  __s32;
typedef uint64_t __u64;
typedef int64_t  __s64;

/* ------------------------------------------------------------------ */
/* <drm.h>: the ioctl encoding, Linux semantics                        */
/* ------------------------------------------------------------------ */

#define SWEET_IOC_NRBITS        8u
#define SWEET_IOC_TYPEBITS      8u
#define SWEET_IOC_SIZEBITS      14u
#define SWEET_IOC_DIRBITS       2u

#define SWEET_IOC_NRSHIFT       0u
#define SWEET_IOC_TYPESHIFT     (SWEET_IOC_NRSHIFT + SWEET_IOC_NRBITS)
#define SWEET_IOC_SIZESHIFT     (SWEET_IOC_TYPESHIFT + SWEET_IOC_TYPEBITS)
#define SWEET_IOC_DIRSHIFT      (SWEET_IOC_SIZESHIFT + SWEET_IOC_SIZEBITS)

#define SWEET_IOC_NONE          0u
#define SWEET_IOC_WRITE         1u
#define SWEET_IOC_READ          2u

#define SWEET_IOC(dir, type, nr, size) \
	(((dir) << SWEET_IOC_DIRSHIFT) | \
	 ((type) << SWEET_IOC_TYPESHIFT) | \
	 ((nr) << SWEET_IOC_NRSHIFT) | \
	 ((size) << SWEET_IOC_SIZESHIFT))

#define DRM_IOCTL_BASE          'd'
#define SWEET_DRM_IO(nr)        SWEET_IOC(SWEET_IOC_NONE, DRM_IOCTL_BASE, (nr), 0)
#define SWEET_DRM_IOR(nr, t)    SWEET_IOC(SWEET_IOC_READ, DRM_IOCTL_BASE, (nr), sizeof(t))
#define SWEET_DRM_IOW(nr, t)    SWEET_IOC(SWEET_IOC_WRITE, DRM_IOCTL_BASE, (nr), sizeof(t))
#define SWEET_DRM_IOWR(nr, t)   SWEET_IOC(SWEET_IOC_READ | SWEET_IOC_WRITE, DRM_IOCTL_BASE, (nr), sizeof(t))

#define DRM_IOCTL_NR(n)         ((n) & 0xff)

/* ------------------------------------------------------------------ */
/* panfrost ioctl numbers (DRM_COMMAND_BASE + n)                       */
/* ------------------------------------------------------------------ */

#define DRM_COMMAND_BASE                        0x40

#define DRM_PANFROST_SUBMIT                     0x00
#define DRM_PANFROST_WAIT_BO                    0x01
#define DRM_PANFROST_CREATE_BO                  0x02
#define DRM_PANFROST_MMAP_BO                    0x03
#define DRM_PANFROST_GET_PARAM                  0x04
#define DRM_PANFROST_GET_BO_OFFSET              0x05
#define DRM_PANFROST_PERFCNT_ENABLE             0x06
#define DRM_PANFROST_PERFCNT_DUMP               0x07
#define DRM_PANFROST_MADVISE                    0x08
#define DRM_PANFROST_SET_LABEL_BO               0x09
#define DRM_PANFROST_JM_CTX_CREATE              0x0a
#define DRM_PANFROST_JM_CTX_DESTROY             0x0b
#define DRM_PANFROST_SYNC_BO                    0x0c
#define DRM_PANFROST_QUERY_BO_INFO              0x0d

/* Generic DRM, not panfrost: what drmCloseBufferHandle() sends. */
#define DRM_GEM_CLOSE                           0x09

#define DRM_IOCTL_PANFROST_SUBMIT       SWEET_DRM_IOW(DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT, struct drm_panfrost_submit)
#define DRM_IOCTL_PANFROST_WAIT_BO      SWEET_DRM_IOW(DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO, struct drm_panfrost_wait_bo)
#define DRM_IOCTL_PANFROST_CREATE_BO    SWEET_DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_CREATE_BO, struct drm_panfrost_create_bo)
#define DRM_IOCTL_PANFROST_MMAP_BO      SWEET_DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_MMAP_BO, struct drm_panfrost_mmap_bo)
#define DRM_IOCTL_PANFROST_GET_PARAM    SWEET_DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_GET_PARAM, struct drm_panfrost_get_param)
#define DRM_IOCTL_PANFROST_GET_BO_OFFSET SWEET_DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_GET_BO_OFFSET, struct drm_panfrost_get_bo_offset)
#define DRM_IOCTL_PANFROST_MADVISE      SWEET_DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_MADVISE, struct drm_panfrost_madvise)
#define DRM_IOCTL_PANFROST_SET_LABEL_BO SWEET_DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_SET_LABEL_BO, struct drm_panfrost_set_label_bo)
#define DRM_IOCTL_PANFROST_JM_CTX_CREATE SWEET_DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_JM_CTX_CREATE, struct drm_panfrost_jm_ctx_create)
#define DRM_IOCTL_PANFROST_JM_CTX_DESTROY SWEET_DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_JM_CTX_DESTROY, struct drm_panfrost_jm_ctx_destroy)
#define DRM_IOCTL_PANFROST_SYNC_BO      SWEET_DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_SYNC_BO, struct drm_panfrost_sync_bo)
#define DRM_IOCTL_PANFROST_QUERY_BO_INFO SWEET_DRM_IOWR(DRM_COMMAND_BASE + DRM_PANFROST_QUERY_BO_INFO, struct drm_panfrost_query_bo_info)
#define DRM_IOCTL_PANFROST_PERFCNT_ENABLE SWEET_DRM_IOW(DRM_COMMAND_BASE + DRM_PANFROST_PERFCNT_ENABLE, struct drm_panfrost_perfcnt_enable)
#define DRM_IOCTL_PANFROST_PERFCNT_DUMP SWEET_DRM_IOW(DRM_COMMAND_BASE + DRM_PANFROST_PERFCNT_DUMP, struct drm_panfrost_perfcnt_dump)
#define DRM_IOCTL_GEM_CLOSE             SWEET_DRM_IOW(DRM_GEM_CLOSE, struct drm_gem_close)

/* ------------------------------------------------------------------ */
/* the structs (verbatim from panfrost_drm.h)                          */
/* ------------------------------------------------------------------ */

#define PANFROST_JD_REQ_FS          (1 << 0)
#define PANFROST_JD_REQ_CYCLE_COUNT (1 << 1)

struct drm_panfrost_submit {
	__u64 jc;
	__u64 in_syncs;
	__u32 in_sync_count;
	__u32 out_sync;
	__u64 bo_handles;
	__u32 bo_handle_count;
	__u32 requirements;
	__u32 jm_ctx_handle;
	__u32 pad;
};

struct drm_panfrost_wait_bo {
	__u32 handle;
	__u32 pad;
	__s64 timeout_ns;
};

#define PANFROST_BO_NOEXEC      1
#define PANFROST_BO_HEAP        2
#define PANFROST_BO_WB_MMAP     4

struct drm_panfrost_create_bo {
	__u32 size;
	__u32 flags;
	__u32 handle;
	__u32 pad;
	__u64 offset;
};

struct drm_panfrost_mmap_bo {
	__u32 handle;
	__u32 flags;
	__u64 offset;
};

enum drm_panfrost_param {
	DRM_PANFROST_PARAM_GPU_PROD_ID,
	DRM_PANFROST_PARAM_GPU_REVISION,
	DRM_PANFROST_PARAM_SHADER_PRESENT,
	DRM_PANFROST_PARAM_TILER_PRESENT,
	DRM_PANFROST_PARAM_L2_PRESENT,
	DRM_PANFROST_PARAM_STACK_PRESENT,
	DRM_PANFROST_PARAM_AS_PRESENT,
	DRM_PANFROST_PARAM_JS_PRESENT,
	DRM_PANFROST_PARAM_L2_FEATURES,
	DRM_PANFROST_PARAM_CORE_FEATURES,
	DRM_PANFROST_PARAM_TILER_FEATURES,
	DRM_PANFROST_PARAM_MEM_FEATURES,
	DRM_PANFROST_PARAM_MMU_FEATURES,
	DRM_PANFROST_PARAM_THREAD_FEATURES,
	DRM_PANFROST_PARAM_MAX_THREADS,
	DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ,
	DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ,
	DRM_PANFROST_PARAM_COHERENCY_FEATURES,
	DRM_PANFROST_PARAM_TEXTURE_FEATURES0,
	DRM_PANFROST_PARAM_TEXTURE_FEATURES1,
	DRM_PANFROST_PARAM_TEXTURE_FEATURES2,
	DRM_PANFROST_PARAM_TEXTURE_FEATURES3,
	DRM_PANFROST_PARAM_JS_FEATURES0,
	DRM_PANFROST_PARAM_JS_FEATURES1,
	DRM_PANFROST_PARAM_JS_FEATURES2,
	DRM_PANFROST_PARAM_JS_FEATURES3,
	DRM_PANFROST_PARAM_JS_FEATURES4,
	DRM_PANFROST_PARAM_JS_FEATURES5,
	DRM_PANFROST_PARAM_JS_FEATURES6,
	DRM_PANFROST_PARAM_JS_FEATURES7,
	DRM_PANFROST_PARAM_JS_FEATURES8,
	DRM_PANFROST_PARAM_JS_FEATURES9,
	DRM_PANFROST_PARAM_JS_FEATURES10,
	DRM_PANFROST_PARAM_JS_FEATURES11,
	DRM_PANFROST_PARAM_JS_FEATURES12,
	DRM_PANFROST_PARAM_JS_FEATURES13,
	DRM_PANFROST_PARAM_JS_FEATURES14,
	DRM_PANFROST_PARAM_JS_FEATURES15,
	DRM_PANFROST_PARAM_NR_CORE_GROUPS,
	DRM_PANFROST_PARAM_THREAD_TLS_ALLOC,
	DRM_PANFROST_PARAM_AFBC_FEATURES,
	DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP,
	DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY,
	DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES,
	DRM_PANFROST_PARAM_SELECTED_COHERENCY,
};

struct drm_panfrost_get_param {
	__u32 param;
	__u32 pad;
	__u64 value;
};

struct drm_panfrost_get_bo_offset {
	__u32 handle;
	__u32 pad;
	__u64 offset;
};

struct drm_panfrost_perfcnt_enable {
	__u32 enable;
	__u32 counterset;
};

struct drm_panfrost_perfcnt_dump {
	__u64 buf_ptr;
};

#define PANFROST_MADV_WILLNEED  0
#define PANFROST_MADV_DONTNEED  1

struct drm_panfrost_madvise {
	__u32 handle;
	__u32 madv;
	__u32 retained;
};

struct drm_panfrost_set_label_bo {
	__u32 handle;
	__u32 pad;
	__u64 label;
};

#define PANFROST_BO_SYNC_CPU_CACHE_FLUSH                        0
#define PANFROST_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE         1

struct drm_panfrost_bo_sync_op {
	__u32 handle;
	__u32 type;
	__u32 offset;
	__u32 size;
};

struct drm_panfrost_sync_bo {
	__u64 ops;
	__u32 op_count;
	__u32 pad;
};

#define DRM_PANFROST_BO_IS_IMPORTED     (1 << 0)

struct drm_panfrost_query_bo_info {
	__u32 handle;
	__u32 extra_flags;
	__u32 create_flags;
	__u32 pad;
};

enum drm_panfrost_jm_ctx_priority {
	PANFROST_JM_CTX_PRIORITY_LOW = 0,
	PANFROST_JM_CTX_PRIORITY_MEDIUM,
	PANFROST_JM_CTX_PRIORITY_HIGH,
};

struct drm_panfrost_jm_ctx_create {
	__u32 handle;
	__u32 priority;
};

struct drm_panfrost_jm_ctx_destroy {
	__u32 handle;
	__u32 pad;
};

/* From drm.h: the argument of DRM_IOCTL_GEM_CLOSE. */
struct drm_gem_close {
	__u32 handle;
	__u32 pad;
};

#endif /* _SWEET_PANFROST_IOCTL_H_ */
