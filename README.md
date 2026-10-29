# KLiOS716

XNU, the KL7 kernel patch train, iOS-root tooling, and the local macOS build VM for the TECNO Spark 30 Pro (KL7, MediaTek MT6789).

This is the parent repository. The ARM64 shim that prepares the handoff and launches XNU is a separate repository:

- https://github.com/lib-fluff-so/KLiOS716-shim

It is included here as the `shim/` git submodule. The shim source is not duplicated in this repository.

## Repository split

This repository owns:

- `xnu/` - the XNU patch train, `build-xnu.sh`, ADT/payload/root helpers, and the verified VM build snapshot;
- `vm/` - the local macOS Sonoma VM, OpenCore configuration, installer helpers, and QMP/OCR tools;
- the iOS root preparation notes and scripts under `xnu/ios-root/` and `rootfs/`;
- the project guide (`REPOSITORY.md`), shim chronology (`SHIM-HISTORY.md`), and historical port notes.

The shim repository owns the bootloader-facing ARM64 code, the on-device test payloads, the UFS/touch/battery branches, and the shim release tags. This parent repository pins the submodule to the `shim-xnu-boot-v1` tag so the XNU build gets the KL7 boot defaults (8 CPUs, iOS-root command line, quiet panel, boot vibration, and no post-handoff PSCI reset).

The XNU kernel and iOS rootfs are **built**, not stored here. Large rootfs images, VM disks, installer media, and build outputs stay outside git.

## Clone

```sh
git clone --recurse-submodules https://github.com/lib-fluff-so/KLiOS716.git
cd KLiOS716
```

For an existing clone:

```sh
git submodule update --init --recursive
```

## Layout

```text
KLiOS716/
├── shim/                     submodule -> KLiOS716-shim (shim-xnu-boot-v1)
├── xnu/                      patch train, build wrapper, ADT/payload/root tools
│   ├── patches/              verified sweet/KL7 patch set
│   ├── vm-build-2026-10-27/  exact VM build snapshot
│   └── ios-root/             offline iOS-root preparation scripts
├── vm/                       local macOS VM/OpenCore/QMP tooling
├── rootfs/                   generated-root notes
├── REPOSITORY.md             detailed repository guide and device safety rules
├── SHIM-HISTORY.md           shim and on-device chronology
├── XNU-PATCH-PORT-PLAN.md    historical XNU port plan
└── FIX-NOTES-*.md            historical shim release notes
```

## Build XNU

XNU is built only in the local macOS VM. There are no GitHub Actions macOS runners.

```sh
# start the VM (prerequisites in vm/README.md)
/home/user/macOS-VM/run/start-vm.sh

# sync the verified patch set into the VM tree, then build there
rsync -a --delete xnu/patches/ \
  dev@127.0.0.1:~/kbuild/ios-sweet/xnu/patches/
scp xnu/build-xnu.sh dev@127.0.0.1:~/kbuild/build-xnu.sh
sshpass -p "$VM_PASS" ssh -p 2222 dev@127.0.0.1 \
  'cd ~/kbuild && env MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT \
   MACHINE_CONFIG=SWEET MAKEFLAGS=-k bash build-xnu.sh'
```

The base patches `0001`-`0081` are from dipa-studio/ios-sweet; the KL7 delta is `0101`-`0117`. There is no `0082` and no `0110` in the verified set. See `xnu/README.md` for the full build and provenance notes.

## Build the shim

The shim is a submodule with its own Makefile and test suite:

```sh
cd shim
make clean
make -j"$(nproc)" all
make hosttest loadertest xnutest
make sizes
```

The pinned `shim-xnu-boot-v1` tag uses the XNU boot defaults. The shim repository's `main` branch remains the v0.10.0 test release.

## Safety

- Flash only `boot_b`; never write `boot_a`.
- Keep `boot_b` at or below 64 MiB.
- Warm-reset log tests must use the built-in PSCI `SYSTEM_RESET`, not fastboot, the power key, or the watchdog.
- Do not add CPU scaling or anything that makes the phone slower.
- Read `REPOSITORY.md` before touching the device or changing the charger/power path.
