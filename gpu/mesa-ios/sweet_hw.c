/*
 * SPDX-License-Identifier: MIT
 *
 * sweet_hw.c: the hardware (gallium panfrost / Mali-G57) pipe_screen of Mesa's
 * OSMesa target on KL7 (MT6789, iOS 16, no DRM).
 *
 * osmesa_create_screen() in osmesa_target.c asks this file first and only
 * falls back to softpipe when it returns NULL:
 *
 *   SWEET_GPU unset or != 0   gallium panfrost over the SweetMali libdrm stand-in
 *   SWEET_GPU=0               force softpipe (the pre-F2 behaviour)
 *
 * The "device" is a file descriptor of the libdrm stand-in (gpu/mali-sweet):
 * every DRM_IOCTL_PANFROST_* it forwards is a selector of the SweetMali user
 * client, and the BO "mmap offsets" it hands out are IOKit memory types.
 * Mesa's os_mmap() is routed to sweet_drm_mmap() (0003-os-mman-sweet.patch),
 * so mapping a BO calls IOConnectMapMemory64() and never the real mmap().
 *
 * Nothing here touches src/panfrost: the screen is created through panfrost's
 * public winsys entry point, exactly the way an EGL/GBM target would.
 *
 * Every step is also logged to the kernel ring (sweet_drm_log): on the device
 * stderr is a file on the iOS root that the host cannot read back, and the
 * failure this has to name -- a silent fallback to softpipe -- only shows up
 * there.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "util/macros.h"
#include "util/xmlconfig.h"
#include "util/driconf.h"
#include "panfrost/drm/panfrost_drm_public.h"

/* gpu/mali-sweet/sweet_drm.h -- the libdrm stand-in, headers not installed */
extern int drmOpenWithType(const char *name, const char *busid, int type);
extern void sweet_drm_log(const char *fmt, ...);
#define SWEET_DRM_NODE_RENDER 2

#define SLOG(...) do { \
   fprintf(stderr, "OSMesa/sweet: " __VA_ARGS__); \
   fprintf(stderr, "\n"); \
   sweet_drm_log("OSMesa/sweet: " __VA_ARGS__); \
} while (0)

/* Proves this file made it into the dylib at all: a build without it has no
 * hook, and osmesa_create_screen() then falls back to softpipe silently. */
__attribute__((constructor))
static void
sweet_hw_loaded(void)
{
   SLOG("hook loaded");
}

/* The dri options panfrost and the gallium core parse at screen creation. */
static const driOptionDescription sweet_driconf[] = {
#include "pipe-loader/driinfo_gallium.h"
#include "panfrost/driinfo_panfrost.h"
};

static struct pipe_screen *g_screen;

struct pipe_screen *sweet_hw_create_screen(void);

struct pipe_screen *
sweet_hw_create_screen(void)
{
   static struct driOptionCache cache;
   static struct driOptionCache info;
   static struct pipe_screen_config config;
   struct pipe_screen *screen;
   const char *e = getenv("SWEET_GPU");
   int fd, err;

   if (g_screen != NULL)
      return g_screen;
   if (e != NULL && e[0] == '0') {
      SLOG("SWEET_GPU=%s -> softpipe on request", e);
      return NULL;
   }

   SLOG("create_screen: enter (SWEET_GPU=%s)", e != NULL ? e : "(unset)");

   errno = 0;
   fd = drmOpenWithType("panfrost", NULL, SWEET_DRM_NODE_RENDER);
   if (fd < 0) {
      err = errno;
      SLOG("drmOpen(panfrost) failed: errno=%d (%s)", err, strerror(err));
      return NULL;
   }
   SLOG("drmOpen(panfrost) = fd %d", fd);

   /* panfrost_create_screen() dereferences config; give it the real thing. */
   driParseOptionInfo(&info, sweet_driconf, ARRAY_SIZE(sweet_driconf));
   driParseConfigFiles(&cache, &info, 0, "panfrost", NULL, NULL, NULL, 0,
                       NULL, 0);
   config.options = &cache;
   config.options_info = &info;

   errno = 0;
   screen = panfrost_drm_screen_create(fd, &config);
   if (screen == NULL) {
      err = errno;
      SLOG("panfrost_drm_screen_create returned NULL: errno=%d (%s)",
           err, strerror(err));
      return NULL;
   }
   SLOG("panfrost screen: %s / %s",
        screen->get_name(screen), screen->get_vendor(screen));
   g_screen = screen;
   return screen;
}
