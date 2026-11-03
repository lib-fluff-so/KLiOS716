# XNU for KL7

This directory is the kernel half of the port: the patch set, the build
wrapper, the port notes, and the offline iOS-root helpers. What each patch
does and what it fixed on the device is in [`HISTORY.md`](HISTORY.md); the
unpolished day-to-day record is [`NOTES.md`](NOTES.md).

## The kernel

- Source: Apple's open-source `xnu-8796` (the iOS 16 generation, macOS 13.5
  era), fetched by [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build)
  pinned at `663906528d1cd5ea8fde7de6f16de73db9511549`.
- Build knobs: `MACOS_VERSION=13.5`, `KERNEL_CONFIG=DEVELOPMENT`,
  `MACHINE_CONFIG=SWEET`.
- There is no `MACHINE_CONFIG=KL7`. The `0082` patch that proposed one was
  written by a model, never compiled and never booted; it is not in this
  repository or its history. The board reuses the `ios-sweet` SWEET
  configuration, and every KL7 difference is an ordinary patch from `0101`
  onwards.

## The patch set

`patches/` is the source of truth. Numbers are the contract: they are the
apply order, and they are never reused.

| Range | What |
| --- | --- |
| `0001`-`0068` | the `ios-sweet` base series: clang compatibility, non-PAC arm64, early framebuffer, physmap, UFS root, AMFI/AppleKeyStore, IOMFB/IOSurface, HID, SMP |
| `0069`-`0081` | the rest of the base series: faster UFS, hibernation, USB probe/ECM, 8-core SMP, display power |
| `0101`-`0117` | the KL7 delta: board/CPU/fbcon matching (`0102`), ramoops console (`0103`-`0104`), RTC (`0105`), MTK UFS (`0107`-`0108`), AppleCLCD (`0109`), sampler (`0111`), FTS3519 touch (`0112`), keys and vibration (`0113`), battery (`0114`), UFS statistics and HS-G3 (`0115`-`0117`, experimental) |
| `0118`-`0136` | SweetMali: the Mali-G57 `IOAccelerator` driver, user client, BOs and GPU VAs, jobs, fences, IOSurface-backed BOs |

Notes on individual numbers:

- `0082`-`0100` is an intentional hole.
- `0101` is corrupt (`corrupt patch at line 83`). The apply step rejects it
  and the build continues without it; it is kept for the record, do not
  renumber it and do not pretend it applies.
- `0110` is retired. The file is
  `0110-kl7-arm-io-display-hierarchy.patch.disabled-v0123`; only `*.patch`
  files are applied, so it stays out of the build but keeps its number.
- `0019`, `0020` and `0026` have a `.sh` sibling: those stage the bundled
  pthread/BearSSL/HFS sources and are copied next to the patches.

## Rules

1. A change exists only as a numbered patch here **and** in the build host's
   `~/kbuild/ios-sweet/xnu/patches/`. A hand-edited build tree is lost on
   the next full build.
2. A new source file is added by the patch, and its `bsd/conf/files` line is
   in the same patch.
3. Do not renumber, do not delete a patch to "clean up", do not squeeze two
   changes into one number.
4. Before flashing, confirm that nothing in the build tree came from
   anywhere else:

   ```sh
   comm -23 \
     <(cd ~/kbuild/dxb/xnu && git status --porcelain | grep '^??' | awk '{print $2}' | sort) \
     <(grep -h '^+++ b/' ~/kbuild/ios-sweet/xnu/patches/*.patch | sed 's|^+++ b/||' | sort -u)
   ```

## Building

Only the local macOS VM is supported ([`../vm/README.md`](../vm/README.md));
there are no hosted macOS runners.

```sh
rsync -a --delete xnu/patches/ dev@127.0.0.1:~/kbuild/ios-sweet/xnu/patches/
scp xnu/build-xnu.sh dev@127.0.0.1:~/kbuild/build-xnu.sh
ssh dev@127.0.0.1 'cd ~/kbuild && MACOS_VERSION=13.5 \
    KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=SWEET bash build-xnu.sh'
```

`build-xnu.sh` does a fresh shallow `dxb` checkout, copies the patch set next
to blacktop's, applies the `ARCHS` edit that lets arm64 through, runs
`build.sh`, and drops the result in `~/kbuild/out/` with a `SHA256SUMS`.
A full build takes minutes; the log is the only place that says which
patches applied.

Once a tree is patched, `build-fast.sh` runs the same `make install` on it
without re-applying anything -- about 20 seconds for a small change. It
compiles whatever is in the tree, so a change tested that way is only real
once it is in a patch.

## Boot payload

The kernel is packed with the shim repository's tools
(`shim/tools/mkpayload_xnu.py`, `shim/tools/mkramdisk.py`), not with the
older `mkpayload_kl7.py` here: the container format belongs to the shim, and
the KL7-specific layout is what `mkpayload_xnu.py` writes.

The ADT matters as much as the kernel. It must describe `kl7-ufs` with base
`0x11270000`; the `v0127` ADT (10624 bytes) is the known-good one. An ADT
with `0x10270000` is what made XNU report
`IOFindBSDRoot: SWEET UFS root ... unavailable: 6` and stop there -- the
kernel was fine, the device tree was not.

`mkadt_kl7.py` and `mkpayload_kl7.py` remain as the reference
implementations of the ADT/payload formats, and `ios-root/` prepares the
root filesystem the kernel mounts.