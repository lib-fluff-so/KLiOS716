/*
 * sweet_drm.h: what the SweetMali libdrm stand-in offers.
 *
 * Two audiences:
 *
 *  - Mesa's panfrost driver, later (F2b).  It includes the *real* libdrm
 *    headers (xf86drm.h, xf86drmSyncobj.h), so the prototypes below are
 *    deliberately identical to libdrm's -- const-ness and all -- and the
 *    stand-in's exported symbols are drop-in replacements.  Nothing in this
 *    header is required to *be* libdrm; it only declares what this library
 *    implements.
 *
 *  - the F2a test, which has no libdrm headers and includes this file to get
 *    both the libdrm-shaped calls and the SweetMali extras at the bottom.
 *
 * The drmVersion and drmDevice layouts match libdrm because Mesa allocates
 * and reads them through pointers we hand out; drmVersion is used by
 * pan_kmod_drm_drv_match() (name + version_major/minor), so its layout is
 * load-bearing.
 */
#ifndef _SWEET_DRM_H_
#define _SWEET_DRM_H_

#include <stddef.h>
#include <stdint.h>

#include "sweet_mali_abi.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* libdrm-shaped API                                                   */
/* ------------------------------------------------------------------ */

/* xf86drm.h */
#define DRM_NODE_PRIMARY        0
#define DRM_NODE_CONTROL        1
#define DRM_NODE_RENDER         2
#define DRM_NODE_MAX            3

#ifndef DRM_CLOEXEC
#define DRM_CLOEXEC             0x00010000  /* == O_CLOEXEC on Darwin */
#endif

typedef struct _drmVersion {
	int version_major;
	int version_minor;
	int version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

/*
 * libdrm's drmDevice.  Only the fields a SweetMali client could plausibly read
 * are faithful (nodes/available_nodes/bustype and the platform busid); the
 * bus-specific unions are spelled out at their real sizes so the struct is
 * the same size as the one Mesa would have allocated.
 */
typedef struct _drmDevice {
	char **nodes;
	int available_nodes;
	int bustype;
	union {
		struct {
			char *busid;
			int domain;
			int bus;
			int dev;
			int func;
			int vendor_id;
			int device_id;
			int subvendor_id;
			int subdevice_id;
			int revision_id;
		} pci;
		struct {
			char *busid;
			unsigned int platform;
		} platform;
		struct {
			char *busid;
			unsigned int protocol_version;
			char *name;
		} usb;
		struct {
			char *busid;
			char *addr;
		} host1x;
	} deviceinfo;
} drmDevice, *drmDevicePtr;

int drmOpen(const char *name, const char *busid);
int drmOpenWithType(const char *name, const char *busid, int type);
int drmClose(int fd);
int drmAvailable(void);
int drmGetNodeTypeFromFd(int fd);

drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);

int drmIoctl(int fd, unsigned long request, void *arg);
int drmCloseBufferHandle(int fd, uint32_t handle);

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
void drmFreeDevices(drmDevicePtr devices[], int count);

/* xf86drmSyncobj.h */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjWait(int fd, const uint32_t *handles, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, const uint32_t *handles,
    uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
int drmSyncobjQuery(int fd, const uint32_t *handles, uint64_t *points,
    uint32_t handle_count);
int drmSyncobjQuery2(int fd, const uint32_t *handles, uint64_t *points,
    uint32_t handle_count, uint32_t flags);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
    uint32_t src_handle, uint64_t src_point, uint32_t flags);

/* DRM_CAP_* names used by the two callers we care about. */
#define DRM_CAP_DUMB_BUFFER             0x1
#define DRM_CAP_VBLANK_HIGH_CRTC        0x2
#define DRM_CAP_PRIME                   0x5
#define DRM_CAP_SYNCOBJ                 0xd
#define DRM_CAP_SYNCOBJ_TIMELINE        0xe

#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL         (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT  (1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE   (1 << 2)

#define DRM_SYNCOBJ_CREATE_SIGNALED     (1 << 0)

/* ------------------------------------------------------------------ */
/* SweetMali extras (not libdrm; the F2a test and the bring-up log)     */
/* ------------------------------------------------------------------ */

/*
 * The device the stand-in reports.  Panfrost's userspace requires >= 1.1
 * (BO flags + the sync provider) and switches on 1.3+ for the system
 * timestamp params, which this kernel answers Unsupported; 1.2 is the newest
 * version whose userspace paths land only on calls the driver implements.
 */
#define SWEET_DRM_VERSION_MAJOR 1
#define SWEET_DRM_VERSION_MINOR 2
#define SWEET_DRM_VERSION_NAME  "panfrost"
#define SWEET_DRM_VERSION_DESC  "SweetMali KL7 (MT6789, Mali-G57, no DRM node)"

/* VERSION, the one selector that does not stand in for an ioctl. */
int sweet_drm_info(struct sm_abi_info *out);

/* Poll VERSION until ready != 0; returns 1 if ready, 0 on timeout. */
int sweet_drm_ready(int timeout_ms);

/* Open the service the way drmOpenWithType() would; -1 on failure. */
int sweet_drm_open(void);

/*
 * mmap()/munmap() a BO that MMAP_BO returned an offset for.  The interposed
 * mmap()/munmap() in sweet_drm.c call these for fds whose object is the
 * stand-in's; naming them separately lets a caller be explicit.
 */
void *sweet_drm_mmap(int fd, size_t len, uint64_t offset);
int sweet_drm_munmap(void *addr, size_t len);

/* JOB_INT_RAWSTAT of the last SUBMIT, for diagnosis. */
uint32_t sweet_drm_last_rawstat(void);

/* printf to stdout and to the kernel ring (SM_SEL_LOG). */
void sweet_drm_log(const char *fmt, ...);

/*
 * E: an IOSurface's pixels as a BO, by IOSurfaceGetID().  Returns 0 and fills
 * *sb (handle + gpu_va + geometry), or -errno.  The handle is an ordinary BO
 * handle; 0 is a valid handle, so check the return, not the handle.
 */
int sweet_drm_surface_bo(struct sm_surface_bo *sb);

/* 1 when tracing is on (SWEETMALI_TRACE=1), else 0. */
int sweet_drm_trace(void);

#if defined(__cplusplus)
}
#endif

#endif /* _SWEET_DRM_H_ */
