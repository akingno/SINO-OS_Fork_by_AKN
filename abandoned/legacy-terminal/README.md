# Archived Ring 0 desktop Terminal

These snapshots are from the G5c desktop implementation at commit `fb8dfcf`.
They preserve the original interactive command editor, shared screen and
scrollback, and banner. The desktop renderer and input routes can be found in
that commit's `Source Code/src32/gui.c`.

The build does not read this directory. The current desktop opens the separate
Ring 3 frontend and Shell. The active source tree retains a smaller panic
console and a headless command evaluator for explicit ShellBridge and kernel
self-test images.
