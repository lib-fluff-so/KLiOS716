# KLiOS716

Apple's **iOS 16 userland running on a TECNO Spark 30 Pro** (KL7, MediaTek
MT6789, Mali-G57). This repository is the kernel side of that port:

- **the XNU patch train** -- Apple's open-source `xnu-8796` (the iOS 16
  generation), taken as a patch series on top of the `ios-sweet` SM7150 port
  and carried forward until it boots this board (`xnu/patches/`, `0001`-`0136`);
- **the build environment** -- a local macOS VM (QEMU + OpenCore), because no
  hosted macOS runner is usable for this, plus the ADT, payload, root-image
  and log helpers in `tools/`;
- **the GPU userspace** -- `gpu/`: a libdrm stand-in over the kernel's
  SweetMali user client, and the Mesa/panfrost build that runs on top of it.

The ARM64 loader (**the shim**) is a separate repository,
[KLiOS716-shim](https://github.com/lib-fluff-so/KLiOS716-shim), attached here
as the `shim/` submodule at a pinned commit. It reads the live DTB and initrd,
prepares memory and `boot_args`, and branches into XNU; it carries no kernel
source.

The phone dual-boots. Android stays on slot `_a` and is the way back; every
image here goes to `boot_b`, and the iOS root lives in the `iosroot`
partition. Nothing is installed by flashing `boot_a`, and recovery/TWRP is
never used.

> Experimental bring-up, not a phone you can use yet. Expect crashes, missing
> apps, and no power management.

## Where it stands

| Area | State |
| --- | --- |
| Kernel | `xnu-8796` boots on the board: six Cortex-A55s, 16 KiB pages, GICv3, PSCI, architected timers, MTK UFS |
| Root | 20H392 (iOS 16.7.16) iPhone X HFS+ root in `iosroot`; launchd and the system daemons run |
| Display | `AppleCLCD` + IOMobileFramebuffer; CoreAnimation renders in software today |
| Input | FTS3519 touch (`0112`), power/volume keys and vibration (`0113`), battery telemetry (`0114`) |
| GPU, kernel | SweetMali: `IOAccelerator` driver, BOs and GPU VAs, jobs, fences, IOSurface-backed BOs (`0118`-`0136`) |
| GPU, userspace | libdrm over the SweetMali user client, Mesa/panfrost, first triangle via OSMesa (`gpu/`) |
| Open | EGL/GLES on top of the panfrost winsys so QuartzCore can use the GPU (F2); DVFS/thermal/suspend (G); a usable jailbroken root |

Kernel builds, on-device runs and what each patch fixes are recorded in
[`xnu/HISTORY.md`](xnu/HISTORY.md). The raw lab notebook, including failed
attempts, is [`xnu/NOTES.md`](xnu/NOTES.md).

## How it boots

```
ABL / LK (the bootloader; unlocked)
  └─ boot_b: shim_b.img                    Android boot image: shim + ramdisk + payload
       └─ shim/  live FDT + initrd -> SWEETXNU payload (Mach-O + ADT + boot_args)
            └─ XNU  MTK UFS -> HFS+ root in `iosroot` -> dyld -> launchd
                 └─ backboardd / SpringBoard -> QuartzCore
```

There is no serial console on this board. The kernel keeps a 4 MiB persistent
log ring at the end of the `iosroot` partition (block 11,000,823; 1025
blocks of 4 KiB), written through on panic. Reading it needs a raw dump of
that partition (the BROM/`antumbra` path used by the bench scripts); a
shim-only failure leaves its console in `/sys/fs/pstore/console-ramoops-0`
for the next Android boot.

## Layout

```
.
├── xnu/                      XNU: the patch train, the build wrapper, ios-root helpers
│   ├── patches/              the patch set, 0001-0136 -- the source of truth
│   ├── build-xnu.sh          the VM build wrapper (fresh dxb + full patch apply)
│   ├── HISTORY.md            what each patch does, build by build
│   ├── NOTES.md              the lab notebook
│   ├── XNU-KL7-PORT-PLAN.md  the original bring-up plan (historical)
│   └── ios-root/             offline iOS root preparation
├── gpu/
│   ├── mali-sweet/           libdrm.2.dylib over SweetMali, and the F1 test client
│   └── mesa-ios/             Mesa/panfrost cross-build and the stage runner
├── vm/                       the macOS build VM (QEMU + OpenCore) and its tools
├── rootfs/                   generated-root policy
├── tools/                    ADT, payload, root image, log and inspection helpers
├── docs/QEMU_TESTING.md      the QEMU harness for the reference `sweet` device
└── shim/                     KLiOS716-shim, pinned (submodule)
```

`tools/` also still carries helpers from the reference `sweet` port
(`mkadt.py`, `qemu-sweet.py`, `sweet-log-pull.sh`, which pulls through
OrangeFox and is **not** the KL7 path).

## Build the kernel

```sh
git clone --recurse-submodules https://github.com/lib-fluff-so/KLiOS716.git
cd KLiOS716
```

The build runs in the macOS VM (`vm/README.md`; SSH on port 2222, Xcode
installed, `~/kbuild` prepared). The patch set is the source of truth, and
`build-xnu.sh` re-fetches the XNU tree and re-applies it:

```sh
rsync -a --delete xnu/patches/ dev@127.0.0.1:~/kbuild/ios-sweet/xnu/patches/
scp xnu/build-xnu.sh dev@127.0.0.1:~/kbuild/build-xnu.sh
ssh dev@127.0.0.1 'cd ~/kbuild && MACOS_VERSION=13.5 \
    KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=SWEET bash build-xnu.sh'
# -> ~/kbuild/out/kernel.development.sweet
```

`MACHINE_CONFIG=SWEET`, not `KL7`: the board reuses the `ios-sweet`
configuration, and the KL7 differences are ordinary patches from `0101` on.
`0101` is corrupt and skipped by the apply step, `0110` is retired, and
`0115`-`0117` are experimental. [`xnu/README.md`](xnu/README.md) has the
rules; `build-fast.sh` gives a ~20 s incremental rebuild of an already
patched tree.

## Build an image and flash it

The payload (kernel + ADT + boot-args) is packed by the shim repository's
tools, because the container format is the shim's:

```sh
# the ADT must be the one whose kl7-ufs node has base 0x11270000 (the `v0127`
# ADT, 10624 bytes). An ADT with 0x10270000 makes the UFS driver probe the
# wrong registers and the kernel never finds its root.
python3 shim/tools/mkpayload_xnu.py KERNEL ADT payload.bin --cmdline "..."
```

Then, with Android up:

```sh
adb reboot bootloader
fastboot flash boot_b  shim_b.img
fastboot flash iosroot iosroot.img      # only when the root image changed
fastboot --set-active=b
fastboot reboot
```

Rules that are not negotiable:

- **Never flash `boot_a`.** `boot_b` only, at most 64 MiB. No recovery, no
  TWRP, no `fastboot boot` into someone else's image.
- The bootloader reports `ro.boot.flash.locked=1` and a green verified-boot
  state. Those properties are spoofed; the bootloader is unlocked and
  flushing works.
- XNU does not reboot on a failed boot. Power-cycle into BROM, run
  `antumbra set-active-slot a`, reboot, and Android is back.

## The GPU

The kernel side is the `0118`-`0136` train (`SweetMali`, an `IOAccelerator`
driver for the Mali-G57). The userspace side is `gpu/`:

- `mali-sweet/` -- `libdrm.2.dylib`, a DRM shim that translates
  `PANFROST_*` ioctls onto the SweetMali user client, plus the F1 test client.
- `mesa-ios/` -- the Mesa cross-build (`libOSMesa.8.dylib`), the `f1_runner`
  daemon that runs the stages on the device, and the tests themselves
  (`f3_test.c`: first triangle, `e_test.c`: the userland half of stage E).

Install names are absolute (`/var/root/libdrm.2.dylib`), because dyld on this
port refuses `@rpath`. Results go to the kernel log ring, so a stage can be
checked from a post-mortem dump.

## Changing the kernel

A change exists only as a numbered patch in `xnu/patches/`. The XNU tree in
the VM is disposable and is rebuilt from the patch set; anything written
there by hand is lost by the next `build-xnu.sh`. Adding a source file means
the file and its `bsd/conf/files` line go into the same patch. Before
flashing, check that no unmanaged file is left in the build tree -- the
command prints nothing when the tree and the patch set agree:

```sh
comm -23 \
  <(cd ~/kbuild/dxb/xnu && git status --porcelain | grep '^??' | awk '{print $2}' | sort) \
  <(grep -h '^+++ b/' ~/kbuild/ios-sweet/xnu/patches/*.patch | sed 's|^+++ b/||' | sort -u)
```

Full rules: [`xnu/README.md`](xnu/README.md).