# iOS rootfs

The iOS rootfs is generated locally and is not stored in this repository.
The preparation scripts, sparse manifest, and userland helpers are in
`xnu/ios-root/`; the main image builder is `xnu/tools/mkiosroot.sh`.

Keep these outside git:

- original OS, SystemOS, AppOS and cryptex images;
- the mounted or copied `IOSRoot` tree;
- raw, sparse, or compressed rootfs images;
- VM disk images and installer media.

Read `xnu/ios-root/README.md` before changing ownership, launchd jobs, or
root-image contents. Work on a copy, never on a mounted live data volume.
