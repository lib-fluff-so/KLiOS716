# Working on KLiOS716

Repository rules for anyone -- human or agent -- changing this tree. The
bench operator's own rules (`~/AGENTS.md` on the build host) win on conflict.

## Kernel changes

- The XNU source of truth is `xnu/patches/`. Never edit the fetched tree by
  hand: a change counts only as a numbered patch, and the same patch must be
  in the build host's `~/kbuild/ios-sweet/xnu/patches/`.
- A new source file needs its `bsd/conf/files` line in the same patch.
- Build with `MACHINE_CONFIG=SWEET`, `MACOS_VERSION=13.5`,
  `KERNEL_CONFIG=DEVELOPMENT`, and read the apply log. `0101` is rejected
  (corrupt) and skipped on purpose, `0110` is retired, `0115`-`0117` are
  experimental.
- Before flashing, run the `comm -23` check in `xnu/README.md`: its output
  must be empty, otherwise the tree holds work the patch set does not
  provide.

## Device

- Flash `boot_b` only. Never `boot_a`, never recovery/TWRP, nothing through
  `fastboot boot`.
- Pack payloads with the `v0127` ADT: `kl7-ufs` base `0x11270000`, 10624
  bytes. An ADT with `0x10270000` leaves XNU without a root.
- XNU does not reboot after a failed boot. Power-cycle to BROM,
  `antumbra set-active-slot a`, reboot, and Android is back.

## Evidence

- A build proves compilation, not boot. A milestone is done when a ring log
  or a hardware marker says so, together with the exact kernel and image
  hashes.
- Keep kernels, images and ring dumps on disk (`/home/fluff_/kl7/artifacts`),
  never in `/tmp`, and keep only the current and the previous build.
- Record results in `xnu/HISTORY.md` (patches, builds) and `xnu/NOTES.md`
  (the lab notebook, including the attempts that failed).

## GPU

- Kernel side: the `0118`-`0136` patches. Userspace side: `gpu/`.
- Mesa changes live in `gpu/mesa-ios/mesa-sweet.patch` and are applied by
  `apply.py`; the panfrost driver itself is not edited in place.
- Install names are absolute (`/var/root/...`). dyld on this port refuses
  `@rpath`, and `__interpose` does not work for dylib references.