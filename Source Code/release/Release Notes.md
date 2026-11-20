# SINO 0.10 / AKN Fork Development Preview

SINO boots from ISO into an 80×25 VGA text Shell. On Windows,
`Start SINO.cmd` uses QEMU and keeps personal files in `Data/data.img`.
The default profile is 4096 MiB RAM and one virtual CPU.

## Capabilities

- Ring 3 Shell and file tools with command history, completion and explicit workspace rights.
- Read-only FAT12 boot volume and automatic workspace snapshots after successful Shell file changes.
- Private process page tables, guarded stacks, checked user buffers, demand-paged anonymous memory and memory-pressure reclamation.
- Native SNX executables, directory grants, streams, pipes, text sessions and blocking waits.
- Loopback IPv4, ICMP, UDP and TCP, with DNS and HTTP checks on loopback.

## Supported configuration

- 32-bit x86 BIOS with standard VGA and PS/2 in QEMU.
- ASCII display and US keyboard input. Files and filenames retain their UTF-8 bytes.
- Up to 64 processes, four threads per process and 24 handles per process.
- VFS supports up to 128 nodes and 64 KiB per file.
- Network services are limited to loopback.

See [Getting Started](<Getting Started.txt>) for the launcher and
[licensing](<../../Documentation/Licenses/Fork-Provenance.md>) for the
project MIT and bundled NASM notices.
