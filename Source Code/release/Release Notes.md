# SINO 0.10 / AKN Fork Development Preview

The current fork boots from ISO and stores personal files on a separate data
disk. The host launcher creates `Data/data.img` on first use and restores an
existing VM window on repeated launches.

## Current capabilities

- Classic graphical desktop with Chinese/English resources and Pinyin input.
- Ring 3 File Manager, Calculator, Clock and SI-SHELL; the text editor and
  settings remain in Ring 0.
- Direct `.snx` execution, `.app` entries and `.lnk` shortcuts.
- Private process page tables, guarded thread stacks, checked user buffers,
  anonymous memory, directory grants, streams, pipes and blocking waits.
- Read-only FAT12 boot volume and explicit `sync` for personal-file snapshots.
- Bounded IPv4, ICMP, UDP, TCP, DNS and HTTP tests on loopback.

## Removed from the current fork

The SysBASIC IDE, task manager, OS installation wizard, software package
installer, application manager and package build/upgrade pipeline have been
removed. F8/F9/F10/F11 no longer launch those tools. This source tree does not
produce a preinstalled system disk or `.sxp` packages. Use the ISO for each boot.
Old upstream release bundles are reference copies and do not describe this fork.

## Known limitations

- BIOS, standard VGA and PS/2 in QEMU; no UEFI, SMP or general hardware support.
- Remaining Ring 0 applications share the kernel's fault domain.
- Eight processes, four threads each, 24 handles per process; VFS has 128 nodes
  and files are limited to 64 KiB.
- Personal files require an explicit commit; keep backups of the data disk.
- Networking is limited to loopback; no external NIC driver or Internet access.
- No Windows, Linux, DOS or VB6 application compatibility. This is a development
  prototype without a security audit.

## Third-party runtime

QEMU, GCC, Node.js and Python are not bundled. See the
[official QEMU download page](https://www.qemu.org/download/#windows).
Licenses and provenance are in `Documentation/Licenses` and
`Source Code/SOURCES.md`.
