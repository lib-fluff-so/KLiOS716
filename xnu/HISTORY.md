# XNU patch and build history

This file records the XNU side of the KL7 port. The shim's version history is
separate; it lives in the KLiOS716-shim repository.

## 1. Base train, `0001`-`0081`

The base series comes from the `ios-sweet` project by dipa-studio. It is kept
as a patch set so the KL7 repository does not import the upstream git
history. The broad groups are:

| Range | Purpose |
| --- | --- |
| `0001`-`0018` | clang compatibility, `MACHINE_CONFIG` support, non-PAC ARM64, open-source pmap/config, private-header stubs, early framebuffer, physmap, debug console, RNG/crypto, device tree and boot CPU |
| `0019`-`0033` | bundled pthread/BearSSL/HFS, mockfs/ramdisk root, UFS root and writes, AMFI/trust-cache, AppleKeyStore, read-write root |
| `0034`-`0049` | UFS DMA, AMFI entitlements, bindfs, watchdog, crash logs, IOMFB/IOSurface, persistent console, devnull tee, IOKit tracing, sandbox policy |
| `0050`-`0068` | crash/sampler work, HFS rename, process sampler, framebuffer present, HID/iPhone identity, touch/power/battery, SMP/PSCI, console mute, UFS speed |
| `0069`-`0081` | UFS faster path, hibernation, touch edges, USB probe/device/ECM/live log, 8-core SMP, display power, net trace, USB link state |

The base series was authored for the sweet/SM7150 target. Many patches are
guarded by `ARM64_BOARD_CONFIG_SWEET`; the KL7 patches below reuse the SWEET
configuration while adding KL7-specific matching and devices. Five base
patches (`0004`, `0010`, `0012`, `0016`, `0018`) were later reduced back to
their generic form, because the KL7 differences they used to guard now live
in `0101`-`0102`; the generic versions are what this repository carries.

## 2. KL7 delta, `0101`-`0117`

The real KL7 patch train starts at `0101`. There is no `0082` in this
repository.

| Patch | Subject | Status |
| --- | --- | --- |
| `0101` | early framebuffer geometry | **known defect**: the apply step rejects it with `corrupt patch at line 83`; kept for the record, not applied |
| `0102` | match platform/CPU/fbcon nodes | applied |
| `0103` | ramoops console mirror | applied |
| `0104` | ramoops early-console replay | applied |
| `0105` | skip RTC wait | applied |
| `0106` | BSD init debug | applied |
| `0107` | MTK UFS | applied |
| `0108` | UFS delay us | applied |
| `0109` | match AppleCLCD display | applied |
| `0110` | arm-io display hierarchy | **retired**: kept as `0110-kl7-arm-io-display-hierarchy.patch.disabled-v0123`; `0109` already creates the display nodes, and the apply loop ignores anything that is not `*.patch` |
| `0111` | sampler trap number | applied |
| `0112` | FTS3519 touch | applied |
| `0113` | keys and vibrator | applied |
| `0114` | battery telemetry and gated slow charging | applied |
| `0115` | UFS I/O statistics | applied (experimental) |
| `0116` | UFS phase benchmark | applied (experimental) |
| `0117` | UFS HS-G3 | applied (experimental) |

The on-device work established that `0102`-`0109` and `0111`-`0114` are
useful; `0115`-`0117` are UFS measurement/performance work and remain
experimental. `0110` stays out of the build. `0101` is still corrupt; do not
pretend it applies. The early panel console was consequently not available
from the kernel until the later display work, and the shim's own framebuffer
console was used for bring-up.

### `0082` is not real

`0082-kl7-machine-config.patch` was a ChatGPT-generated "KL7 machine config"
patch that does not build. It was removed on 2026-10-29 from the host
`ios-sweet` working tree, the VM source trees, the Downloads artifact, and
this consolidated repository. It is not in this repository's history either:
the old `ios-sweet` history that added it was not imported. The KL7 changes
that actually work are the `0101`+ patches, not a new `MACHINE_CONFIG=KL7`.

## 3. GPU train, `0118`-`0136` (SweetMali)

The board has a Mali-G57, which is neither an Apple GPU nor a DRM device.
`0118`-`0136` add a kernel driver that exposes the ring buffer, the job slots,
the MMU and memory objects to one user client, and `../gpu/` turns that into
the DRM interface Mesa's panfrost winsys expects. The driver is an
`IOAccelerator`; the userland side speaks `drmIoctl`.

| Patch | Subject | Evidence from the device |
| --- | --- | --- |
| `0118` | the IOAccelerator driver for the Mali-G57 | probes, reads the GPU version, brings the ring up |
| `0119` | publish the service and its user client | `IOService` + user client, `GET_PARAM` (D1) |
| `0120` | `SUBMIT`: a client job descriptor onto the JM | job runs, target value written (D2) |
| `0121` | fences: `WAIT_BO`, syncobj wait, reset and recovery | D3 32/32; an MMU fault latches in `MMU_INT_RAWSTAT`, not in the job slot |
| `0122` | `MMAP_BO`: a BO mapped into the client task | 16 KiB written and read back at offsets 0 and 0x1000 (B2) |
| `0123` | F1 install: carry the test blob and run it | byte-wise copy out of Device memory -- vectorised loads fault |
| `0124` | start the driver from the platform expert | driver starts without a kext load |
| `0125` | SweetLog: write through on panic so the ring survives | the ring survives a panic |
| `0126` | gate the user client on the GPU being up | no user client before the ring runs |
| `0127` | unmap a BO when its handle is closed | no leaked VA |
| `0128` | AKS: rate-limit the log | the 4 MiB ring keeps ~45 min instead of ~7 min of AKS noise |
| `0129` | a 32 MiB F1 blob, the installer writes the files | `ml_io_map_unmappable` (`io_submap` is 8 MiB); 128 KiB chunks |
| `0130` | BO memory does not have to be contiguous | scattered BOs, the way panfrost allocates |
| `0131` | syncobj create-signalled | panfrost needs a signalled syncobj to build a context |
| `0132` | `SUBMIT` validates the client's BO handles | bad handle -> `EINVAL`, no MMU fault |
| `0133` | every BO gets its own GPU VA | first-fit VA allocator with guard pages (A3) |
| `0134` | the client's job slot and AS go into the job | more than one address space (A4) |
| `0135` | surface-backed BOs: an IOSurface's pages in the GPU AS | stage E 31/31: `SUBMIT` writes the surface's own pixels; the client's window sees the same bytes |
| `0136` | a surface-backed BO keeps its window when the handle closes | found by E2: `CLOSE_BO` used to drop the surface's mapping |

Confirmed on the board: kernel stage E 31/31, userland E2 13/13
(`../gpu/mesa-ios/e_test.c`), D1/D2/D3 green, F1 21/21 plus user client 32/32,
and F3 -- `GL_RENDERER = Mali-G57 (Panfrost)`, the first triangle drawn on the
GPU through Mesa.

## 4. Build chronology

| Date | Build / kernel | Patches | Notes |
| --- | --- | --- | --- |
| 2026-10-25 | `6689ce2e...` (v0113/v0114) | `0101`-`0111`; `0101` skipped | VM build; early console still targeted sweet's framebuffer address, so the KL7 panel stayed blank; `0111` sampler format string verified in the binary |
| 2026-10-26 | `6bca36e9...` (v0121 touch) | adds `0112` | FTS3519 touch works on hardware; only `boot_b` flashed |
| 2026-10-26 | `50eb4e5e...` (v0123 keys) | adds `0113` | volume/power keys and boot vibration work on hardware |
| 2026-10-27 | `1bfe9cd7...` | `0102`-`0109`, `0111`-`0117`; `0101` skipped; no `0110` | battery `0114` and UFS `0115`-`0117` included |
| 2026-11-02 | `kernel-v41` | adds `0118`-`0135` | SweetMali: F1 21/21, D1-D3, stage E 31/31, F3 first triangle through OSMesa |
| 2026-11-03 | `kernel-v43` | adds `0136` | E2 13/13 from a real process; `CLOSE_BO` keeps a surface's window |

Kernel and image hashes for each device run are kept with that run's
artifacts, outside git. Only the current and the previous build are kept.

## 5. Build environment

The only supported build path is the local macOS VM:

- macOS Sonoma 14.6, QEMU/KVM, Skylake-Client CPU model.
- Xcode at `/Applications/Xcode.app/Contents/Developer`.
- `MACOS_VERSION=13.5`, `KERNEL_CONFIG=DEVELOPMENT`, `MACHINE_CONFIG=SWEET`.
- blacktop `darwin-xnu-build` pinned at
  `663906528d1cd5ea8fde7de6f16de73db9511549`.
- XNU source SHA in the last log: `1b191cb58250d0705d8a51287127505aa4bc0789`.
- Output: `~/kbuild/out/kernel.development.sweet`.

There are no hosted macOS runners. The old `xnu.yml` workflow is gone;
`build-xnu.sh` is its replacement.

The wrapper `build-xnu.sh` does a fresh shallow `dxb` checkout, copies
`ios-sweet/xnu/patches/*.patch` and `*.sh`, applies the local `ARCHS` edit,
runs blacktop's `build.sh`, and copies the kernel artifacts. `build-fast.sh`
is the incremental path: it recompiles an already patched `dxb` tree, which
is why a change that is not a patch can pass a test and be lost.

## 6. Keeping the VM in step

`xnu/patches/` in this repository is the source of truth. The build host has
the copy the build consumes:

```sh
rsync -a --delete xnu/patches/ dev@127.0.0.1:~/kbuild/ios-sweet/xnu/patches/
```

`build-xnu.sh` re-fetches `dxb` and re-applies the whole set, so a patch that
does not apply fails the run. Before flashing, check that nothing exists only
in the build tree -- the command prints every untracked file the patch set
does not provide:

```sh
comm -23 \
  <(cd ~/kbuild/dxb/xnu && git status --porcelain | grep '^??' | awk '{print $2}' | sort) \
  <(grep -h '^+++ b/' ~/kbuild/ios-sweet/xnu/patches/*.patch | sed 's|^+++ b/||' | sort -u)
```

Every change must also exist as a numbered patch, and a new source file needs
its `bsd/conf/files` line in the same patch.

## 7. iOS root and userland

The XNU kernel work is one half of the port. The root userland is prepared
offline with `ios-root/post-root.sh` and the `v18-tweaks` scripts:

- `mkiosroot.sh` builds a single HFS+ volume from the iOS OS and cryptex
  images, preserves owners/modes/hard links, creates the shared-cache links,
  logd stores, and the no-passcode system keybag.
- `post-root.sh` repeats the VM-side/offline fixups for
  `/Users/dev/ios/IOSRoot`.
- `sweet-root-tweaks.py` applies the iOS 16.7.16 bring-up edits: launchd
  panic behavior, containers, migration marker, audio/MediaToolbox workarounds,
  SpringBoard watchdogs, setup flags, and activation.
- `sweet_daemons.py` can inspect or restore the launchd jobs disabled during
  bring-up; it does not implement their hardware dependencies.
- `v18-container.py` seeds the `mobileactivationd` system container.
- `v19-migrate.py` forces a real DataMigration for the offline root.
- `make_sparse.py` converts a raw root image to Android sparse format.

The `ios-root/patch-v17/manifest.tsv` is only a manifest for nine 4 MiB
sparse chunks. The chunk blobs were not preserved; see
`ios-root/patch-v17/README.md`.

Making the phone usable -- a jailbroken root, Sileo, a terminal, the app
registration problem -- is a separate track from the kernel and the GPU. It
is tracked with the rootfs images, not in this patch train.

## 8. Validation and open work

- The kernel boots iOS 16.7.16 on the board: UFS root, display, touch, keys
  and battery work on hardware, and only `boot_b` is ever flashed.
- The GPU is green in the kernel and in userland: stage E 31/31, E2 13/13,
  D1/D2/D3, F1 21/21 and user client 32/32, and F3, the first triangle
  through Mesa/panfrost.
- Open, in order: F2 -- an EGL/GLES surface on top of the panfrost winsys so
  QuartzCore's `EAGLContext` path can render on the GPU instead of the
  software renderer; then power management (DVFS, thermal, suspend) with its
  own fail-fast design; then the rootfs track above.
- `0101` is corrupt and skipped. `0110` is retired. `0115`-`0117` are
  experimental.
- A build proves compilation, not boot. Nothing is verified without a ring
  log from the device.

## 9. References

- `../README.md` - repository overview: build, flashing, device rules
- `README.md` - patch set rules and build instructions
- `patches/` - the patch train itself
- `NOTES.md` - detailed lab notebook, including the 0101/0110 history
- `XNU-KL7-PORT-PLAN.md` - the original plan; historical
- `../gpu/` - the userspace side of SweetMali (libdrm, Mesa/panfrost)
- `../vm/README.md` - the build VM