# KL7 repository guide

This is the parent repository for the TECNO Spark 30 Pro (KL7, MediaTek
MT6789) XNU port. It owns the XNU patch train, the iOS-root helpers, the
local macOS VM build tooling, and the project documentation.

The ARM64 shim is a separate repository and is included here as a git
submodule:

```text
https://github.com/lib-fluff-so/KLiOS716-shim -> shim/
```

The shim prepares the live DTB/initrd, places the `SWEETXNU` payload, builds
`boot_args`, and launches XNU. The kernel and rootfs are built around it in
this repository.

## Start here

| Document | Contents |
| --- | --- |
| `README.md` | parent repository overview and quick start |
| `REPOSITORY.md` | this guide: layout, builds, device rules |
| `xnu/README.md` | authoritative XNU patch set, VM build, sync/build commands |
| `xnu/HISTORY.md` | XNU patch chronology and host/VM divergence |
| `vm/README.md` | local macOS VM launcher, install flow, QMP tools |
| `xnu/ios-root/README.md` | offline iOS root and userland helpers |
| `rootfs/README.md` | generated-root policy |
| `SHIM-HISTORY.md` | shim and on-device chronology |
| `XNU-PATCH-PORT-PLAN.md` | historical XNU port plan |
| `FIX-NOTES-*.md` | historical shim release notes |

## Repositories and branches

- `KLiOS716` - this parent repository. `main` is the default branch.
- `KLiOS716-shim` - the shim source, tests, topic branches, and release tags.
  - `main` - v0.10.0 test release.
  - `ufs-test` - UFS-only work.
  - `touch-test` - touch-only work.
  - `battery-test` - combined battery and charger work.
  - `shim-xnu-boot-v1` - the shim commit pinned by this repository's
    submodule. It adds the XNU boot defaults without carrying kernel source.

The shim repository deliberately has no XNU branch: XNU patches, the VM,
and the rootfs tooling live here.

## Layout

```text
.
├── shim/                     shim submodule (KLiOS716-shim)
├── xnu/                      XNU patch set, build wrapper, ADT/payload/root tools
│   ├── patches/              verified sweet/KL7 patch set
│   ├── vm-build-2026-10-27/  exact VM build snapshot
│   ├── ios-root/             offline iOS-root preparation
│   ├── tools/                ADT, payload, root, inspection utilities
│   └── out/SHA256SUMS        latest verified kernel hash
├── vm/                       local macOS VM, OpenCore, QMP/OCR tools
├── rootfs/                   generated-root notes
├── SHIM-HISTORY.md
├── XNU-PATCH-PORT-PLAN.md
├── FIX-NOTES-*.md
└── REPOSITORY.md
```

Large generated artifacts (`xnu/out*`, VM disks, installer media, generated
OpenCore images, phone dumps, rootfs images) are intentionally not tracked.
Keep permanent copies outside git.

## Checkout

```sh
git clone --recurse-submodules https://github.com/lib-fluff-so/KLiOS716.git
cd KLiOS716
```

For an existing checkout:

```sh
git submodule update --init --recursive
```

The `shim/` submodule is pinned to `shim-xnu-boot-v1`. Update it only with a
deliberate commit that records the new shim SHA.

## Shim build and test

The shim has its own Makefile and host tests:

```sh
cd shim
make clean
make -j"$(nproc)" all
make hosttest loadertest xnutest
make sizes
```

The main artifact is `shim/out/shim_b.img`; `shim/out/shim_raw_b.img` is the
uncompressed test variant. Battery/charger and topic-specific builds are
documented in the shim repository. The parent repository does not duplicate
the shim source.

## XNU build

Only the local macOS VM is supported. There are no GitHub Actions macOS
runners.

The verified patch set is `xnu/patches/`: 97 sweet/KL7 patches and three
staging scripts, with no `0082` and no `0110`. The exact VM snapshot is in
`xnu/vm-build-2026-10-27/`.

```sh
# start the VM (see vm/README.md for prerequisites)
/home/user/macOS-VM/run/start-vm.sh

# sync the verified patch set and wrapper
rsync -a --delete xnu/patches/ \
  dev@127.0.0.1:~/kbuild/ios-sweet/xnu/patches/
scp xnu/build-xnu.sh dev@127.0.0.1:~/kbuild/build-xnu.sh

# build
sshpass -p "$VM_PASS" ssh -p 2222 dev@127.0.0.1 \
  'cd ~/kbuild && env MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT \
   MACHINE_CONFIG=SWEET MAKEFLAGS=-k bash build-xnu.sh'
```

The last verified kernel is
`1bfe9cd7b07d8fd14ca6e068bf93f776bfafecf5dc0797bfd6c796ac003c86ff`.
`vm/out/SHA256SUMS` contains an older copy and is not the latest build.
`xnu/patches/0101` is known-corrupt and is skipped by `build.sh`; inspect the
log. `0115`-`0117` are experimental. Do not restore `0082`.

## iOS rootfs

The rootfs is built locally, not checked in. Start with
`xnu/ios-root/README.md` and `rootfs/README.md`; the main image builder is
`xnu/tools/mkiosroot.sh`. Keep original OS/cryptex images and a known-good
root outside git.

## Current device state

- Android lives on slot `_a`; it is the fallback.
- Shim/XNU test images go to `boot_b` only.
- The last tested `boot_b` image is the unique-sentinel/vendor `JS_CONFIG`
  image; no new shim image was flashed during the history rebuild.
- Fastboot flow: `adb -s $DEVICE_SERIAL reboot bootloader`, then
  `fastboot flash boot_b .../shim_b.img`, `fastboot --set-active=b`,
  `fastboot reboot`. The slot-B retry window is about 90 seconds.
- From iOS, start Antumbra first and wait for the transient BROM port.
- Warm-reset log tests must end with the built-in PSCI timer; a fastboot,
  power-key, or watchdog reset loses the shim ramoops.

### Hard safety rules

- **Never flash `boot_a`.** Only `boot_b`.
- **`boot_b` must stay at or below 64 MiB.**
- Do not use `/home/user/Downloads/recovery.img` or
  `/home/user/TWRP/recovery.img`.
- The battery/charger path is read-only by default; slow charging is gated
  behind `kl7slowchg=1`. Do not leave an unsafe charger setting on the device.
  The shim's guarded write test disables OTG, checks the fault register, and is
  not the default.
- Do not add CPU scaling or anything that makes the phone slower.
- Use host `sudo` normally; no password is stored in this repository.

## Source and artifact policy

- XNU base patches are from dipa-studio/ios-sweet; the KL7 delta is
  `0101`-`0117`. The old `ios-sweet` git history is not imported.
- The shim source and release history live in `KLiOS716-shim`; this parent
  repository consumes the shim as a submodule.
- The public repositories contain only the rebuilt shim topic refs and this
  XNU/VM tree. The private pre-rewrite backup and bundle are not published.
- Keep `/tmp` test artifacts that matter under `/home/user/kl7/` or in
  `opencode-archive/`; `/tmp` is not permanent.

## What is not in git

- VM disks and installer media (`/home/user/macOS-VM/img/*`).
- Generated OpenCore image (`vm/OpenCore/OpenCore.qcow2`).
- Phone partition dumps, boot images, pstore logs, and build outputs.
- The `patch-v17` chunk blobs; only the manifest survived.
- The original `ios-sweet` history and its abandoned `0082` commit.
- Large iOS rootfs images; only the preparation scripts and manifest are here.

## Verification checklist after a change

1. Shim: `cd shim && make clean && make -j$(nproc) all && make hosttest loadertest xnutest`.
2. XNU: sync `xnu/patches/`, build in the VM, inspect every patch apply/skip,
   confirm the kernel link, record the hash.
3. Device: flash only `boot_b`, keep Android on `_a`, confirm the slot-B retry
   window, pull `console-ramoops-0` before the next boot, and record the exact
   image/hash.
4. Update `SHIM-HISTORY.md`, `xnu/HISTORY.md`, and the relevant shim branch
   with the result. Do not leave a successful test only in `/tmp`.
