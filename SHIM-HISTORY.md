# Shim history

This is the history of the `ios-kl7` ARM64 shim, not the XNU patch train.
The shim boots from an Android boot image, talks to the live KL7 firmware
state, and hands off either a test payload or the `SWEETXNU` container.
The kernel-side history is in `xnu/HISTORY.md`.

The shim source now lives in
[`KLiOS716-shim`](https://github.com/lib-fluff-so/KLiOS716-shim). This parent
repository consumes it as the `shim/` submodule and owns the XNU patches, the
macOS VM, and the iOS-root tooling.

## Reconstructed release history

The old repository had the release source only as a pile of Downloads
archives plus a separate `ufs-test` branch. The history was rebuilt from
those archives in order, then the existing 46 UFS/touch/battery commits were
replayed on top. The reconstructed v0.10.0 tree is the public `v0.10.0` release
(`fd42085`); the replayed tree matched the pre-consolidation branch tip
before the consolidation commit.

| Tag | Date (2026) | Commit | What it added |
| --- | --- | --- | --- |
| `v0.5` | 10-21 00:33 | `bbf1fa0` | initial EL1 bring-up: framebuffer diagnostics, CPU/DTB/bootargs/PSCI report, 2 KiB-aligned exception vectors, ESR/FAR/ELR/SPSR dump |
| `v0.5.1` | 10-21 00:48 | `5562e76` | fix SP_EL1 selection and exception vectors; emulator compatibility and capture offset |
| `v0.5.2` | 10-21 01:36 | `9f57061` | ramoops console mirror; source archive was not preserved, so this commit keeps the v0.5.1 tree plus the release note and both surviving images |
| `v0.6.0` | 10-21 17:43 | `615cc1d` | identity-map live DTB with 16 KiB granule, parse memory/reservations/GIC/PSCI/8 CPUs, MMU on, PSCI secondaries, host tests |
| `v0.6.1` | 10-21 17:51 | `c579b34` | secondary-core framebuffer markers and AFFINITY_INFO diagnostics |
| `v0.6.2` | 10-21 17:54 | `764a29a` | clean/invalidate secondary context before `CPU_ON`; stage colours |
| `v0.6.3` | 10-21 17:59 | `f21c6af` | build per-core context from a PC-relative base; record received `x0` |
| `v0.7.0` | 10-21 21:12 | `2bd05cd` | ramoops-first UI, test-payload handoff, CPIO/DTB parsing, high-RAM preflight, result ABI |
| `v0.7.1` | 10-21 21:29 | `af59ed4` | fix the handoff result marker (`0x5345`, not `0x4553`) |
| `v0.8.0` | 10-21 22:39 | `b644566` | optional `SWEETXNU` loader: Mach-O segments/BSS, ADT, `boot_args` rev 2, host packer/tests |
| `v0.8.0-ctf-hotfix` | 10-21 22:57 | `ebce16b` | accept the non-mapped `__CTF` segment with `vmsize == 0` |
| `v0.9.2` | 10-22 17:56 | `6aa668e` | v0.9.0 scope reduction and v0.9.1 loader contract; watchdog diagnostics/disable; aligned XNU copies. No separate v0.9.1 source archive was preserved |
| `v0.9.3` | 10-23 21:45 | `52a4501` | XNU framebuffer base `0xFE519E00`; add the early `patches-kl7` staging tree (0101-0103) |
| `v0.10.0` | 10-24 11:36 | `fd42085` | PSCI `CPU_ON` timer on cpu1 with `SYSTEM_RESET` after 50 s, WDT backstop, `VISIBLE_OFFSET 0`; old `main` tree |

### Source gaps and image-only releases

- **v0.5.2** has no source archive. The commit preserves
  `FIX-NOTES-v0.5.2.md` and the two 16 KiB images under
  `releases/v0.5.2/`; the source tree is v0.5.1. v0.6.0 is the first source
  snapshot that contains the ramoops implementation.
- **v0.9.0 and v0.9.1** have no separate source archives. The v0.9.2 archive
  is the first compact-loader source snapshot and its message covers the
  v0.9.0/v0.9.1 scope.
- **v0.10.0** is the public release tree (`fd42085`). It does not
  include the separate v0.9.3 `patches-kl7` staging directory; the XNU work
  continued outside this repository until the consolidation.

## Shim topic branches after v0.10.0

The shim work after v0.10.0 is split by topic in `KLiOS716-shim`, and every
branch starts from its `main`:

- `ufs-test` - only the UFS probe, UFS ADT host, iosroot boot arguments,
  HS-G1/throughput, PRDT reads, and residual accounting.
- `touch-test` - only the SPI1/clock/GPIO bring-up, FTS3519 framing, report
  parsing, and live crosshair test.
- `battery-test` - the read-only battery AUXADC and SGM41516D probes plus
  the guarded SGM41516D charger-write test. Charger work lives here; there is
  no separate `charger-test` branch.

The XNU boot defaults are not a shim branch. This parent repository pins the
`shim/` submodule to the `shim-xnu-boot-v1` tag: eight CPUs, the iOS-root
command line, the quiet panel, the boot vibration pulse, and the optional
PSCI timer disabled for the XNU handoff.

`main` in the shim repository remains the v0.10.0 release and its default
branch. The charge-write test is deliberately gated: it is not the default
battery path, it refuses to write on a fault, it never enables OTG, and it
leaves the safety timer disabled only as an explicit test setting. iOS
charging must be boot-arg gated (`kl7slowchg=1`) and must pet the SGM
watchdog.

## Current working tree

After the release history, the shim gained the UFS/touch/battery work above.
The XNU patch train, VM tooling, and iOS-root helpers now live in this parent
repository. GPU bring-up remains private work and is not part of the public
tree.

The last hardware test image on `boot_b` is the unique-sentinel/vendor
`JS_CONFIG` image; Android remains on slot `_a`. No new shim image was
flashed during the history rebuild.

## History mechanics

- The rebuilt history was made in a throwaway clone, not by rewriting the
  real repository in place.
- The 11 `ufs-*`/`xnu-*` tags were recreated by matching the original commit
  subjects. `xnu-ufs-iosroot1` is annotated again with the original message
  and tagger; the other tags were lightweight originally and remain so.
- The public shim repository publishes only the rebuilt v0.10.0 line and the
  three topic branches. This parent repository publishes the XNU/VM/rootfs
  work. The private pre-rewrite backup is not published.
- The `ios-sweet` git history is not imported. See `xnu/HISTORY.md` for the
  base-patch attribution and the removed `0082` hallucination.

## Safety rules

- Flash **only** `boot_b`. Never write `boot_a`.
- Keep `boot_b` at or below 64 MiB.
- Prefer fastboot while Android is running:
  `adb -s $DEVICE_SERIAL reboot bootloader`, then
  `fastboot flash boot_b ...`, `fastboot --set-active=b`, `fastboot reboot`.
- From iOS, start Antumbra first and wait for the transient BROM port.
- End warm-reset log tests with the built-in PSCI timer, not a fastboot,
  power-key or watchdog reset. A cold reset loses the shim ramoops.
- Do not use the Downloads or TWRP `recovery.img` artifacts.
- Do not add CPU scaling or other changes that make the phone slower.

## Related files

- [`KLiOS716-shim`](https://github.com/lib-fluff-so/KLiOS716-shim) - shim source, tests, topic branches, and release tags
- `README.md` - parent repository overview
- `REPOSITORY.md` - repository map and current operating rules
- `xnu/HISTORY.md` - XNU patch/build history
- `xnu/README.md` - XNU build and sync
- `xnu/ios-root/README.md` - offline iOS-root preparation
- `rootfs/README.md` - generated-root policy
