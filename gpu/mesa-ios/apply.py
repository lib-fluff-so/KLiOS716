#!/usr/bin/env python3
"""Apply the SweetMali changes to the Mesa 25.0.7 tree (run inside ~/f2build/mesa)."""
import sys, pathlib

ROOT = pathlib.Path(".")

def sub(path, old, new, count=1):
    p = ROOT / path
    s = p.read_text()
    if s.count(old) != count:
        sys.exit(f"{path}: expected {count} occurrence(s), found {s.count(old)}:\n{old[:200]}")
    p.write_text(s.replace(old, new, count))
    print(f"patched {path}")

# ---------------------------------------------------------------- osmesa target
sub("src/gallium/targets/osmesa/osmesa_target.c",
"""struct pipe_screen *
osmesa_create_screen(void);
""",
"""struct pipe_screen *
osmesa_create_screen(void);

/* SweetMali: hardware path (targets/osmesa/sweet_hw.c) */
struct pipe_screen *sweet_hw_create_screen(void) __attribute__((weak));
""")

sub("src/gallium/targets/osmesa/osmesa_target.c",
"""   struct sw_winsys *winsys;
   struct pipe_screen *screen;

   /* We use a null software winsys since we always just render to ordinary""",
"""   struct sw_winsys *winsys;
   struct pipe_screen *screen;

   /* SweetMali: gallium panfrost when the SweetMali device is there */
   screen = sweet_hw_create_screen ? sweet_hw_create_screen() : NULL;
   if (screen)
      return debug_screen_wrap(screen);

   /* We use a null software winsys since we always just render to ordinary""")

# ---------------------------------------------------------------- osmesa build
sub("src/gallium/targets/osmesa/meson.build",
"""osmesa_def = custom_target(""",
"""osmesa_sweet_files = []
osmesa_sweet_deps = []
if with_gallium_panfrost and host_machine.system() == 'darwin'
  osmesa_sweet_files += 'sweet_hw.c'
  osmesa_sweet_deps += [driver_panfrost, dep_libdrm]
endif

osmesa_def = custom_target(""")

sub("src/gallium/targets/osmesa/meson.build",
"""  'osmesa_target.c',
""",
"""  ['osmesa_target.c', osmesa_sweet_files],
""")

sub("src/gallium/targets/osmesa/meson.build",
"""    dep_ws2_32, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,
""",
"""    dep_ws2_32, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,
    osmesa_sweet_deps,
""")

# ---------------------------------------------------------------- GLES profile
sub("src/gallium/frontends/osmesa/osmesa.c",
"""         if (profile != OSMESA_CORE_PROFILE &&
             profile != OSMESA_COMPAT_PROFILE)
            return NULL;""",
"""         if (profile != OSMESA_CORE_PROFILE &&
             profile != OSMESA_COMPAT_PROFILE &&
             profile != 0x36 /* SweetMali: OpenGL ES */)
            return NULL;""")

sub("src/gallium/frontends/osmesa/osmesa.c",
"""   attribs.profile = (profile == OSMESA_CORE_PROFILE)
      ? API_OPENGL_CORE : API_OPENGL_COMPAT;
   attribs.major = version_major;""",
"""   attribs.profile = (profile == OSMESA_CORE_PROFILE)
      ? API_OPENGL_CORE : API_OPENGL_COMPAT;
   if (profile == 0x36) /* SweetMali: GLES, major 1 = ES1, else ES2/ES3 */
      attribs.profile = (version_major == 1) ? API_OPENGLES : API_OPENGLES2;
   attribs.major = version_major;""")

# ---------------------------------------------------------------- os_mman hook
sub("src/util/os_mman.h",
"""#if DETECT_OS_ANDROID && !defined(__LP64__)""",
"""#if defined(__APPLE__)
/*
 * SweetMali (KL7): the panfrost "DRM device" is a libdrm stand-in over an
 * IOKit user client (gpu/mali-sweet).  Its BO mappings are not real mmap()s,
 * so every os_mmap()/os_munmap() is offered to the stand-in first; it falls
 * back to the C library for fds and addresses it does not own.  The symbols
 * are weak, so Mesa still links and runs without the stand-in (softpipe).
 */
#include <stdint.h>

void *sweet_drm_mmap(int fd, size_t len, uint64_t offset)
   __attribute__((weak));
int sweet_drm_munmap(void *addr, size_t len) __attribute__((weak));

static inline void *
os_mmap_sweet(void *addr, size_t length, int prot, int flags, int fd,
              off_t offset)
{
   if (sweet_drm_mmap)
      return sweet_drm_mmap(fd, length, (uint64_t)offset);
   return mmap(addr, length, prot, flags, fd, offset);
}

static inline int
os_munmap_sweet(void *addr, size_t length)
{
   if (sweet_drm_munmap)
      return sweet_drm_munmap(addr, length);
   return munmap(addr, length);
}

#  define os_mmap(addr, length, prot, flags, fd, offset) \\
             os_mmap_sweet(addr, length, prot, flags, fd, offset)

#  define os_munmap(addr, length) \\
             os_munmap_sweet(addr, length)

#elif DETECT_OS_ANDROID && !defined(__LP64__)""")

print("all hunks applied")
