# Working on the KL7 XNU port

- Read `XNU-KL7-PORT-PLAN.md` first, then the active handoff at the top of `NOTES.md`. The target is Tecno Spark 30 Pro KL7 / MediaTek MT6789 and iOS 16-era `xnu-8796`; Linux ports are out of scope.
- Treat this tree as a KL7 bring-up overlay based on `ios-sweet`, not as an upstream-ready patch series. `ARM64_BOARD_CONFIG_SWEET` is temporarily reused to compile the common iOS-on-MacOS compatibility layer; do not assume every sweet-named hardware driver applies to KL7.
- Read the relevant patches in order before editing. Distinguish patch syntax parsing from actually applying patches to xnu-8796 and from compiling XNU. Never report the latter two as passed without a CI/build-tree result.
- Keep the first device run deliberately small: actual XNU Mach-O, KL7 ADT generated without UFS or `/product`, `cpus=1`, `rd=md0`, WDT disabled at shim handoff. Required evidence is an XNU-originated marker or panic state captured in ramoops; a synthetic shim payload is not XNU validation.
- Update the active handoff in `NOTES.md` after each milestone. Record exact source revision, CI run, build result, artifact names/hashes and device log facts.
- Do not touch the user's phone, flash partitions, or run destructive actions as part of local build prep. Preserve Android and backups. USB/iBoot menu, recovery UI, UFS root and SMP come later.
- Keep device dumps, serial numbers and downloaded Apple artifacts out of git.
