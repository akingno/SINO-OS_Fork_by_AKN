# Desktop Shell after G6

F2, the SINO menu's Terminal item, and `/Applications/Terminal.app` open or
focus the same writable **SINO Shell** window. F12 opens a separate read-only
session. From a writable Shell, `run /boot/term.snx` opens a read-only window
and `run /boot/term.snx /write` explicitly opens another writable window.
The frontend and strict command interpreter run in separate Ring 3 processes.
Each window owns its scrollback, input line, command history and current directory.

The logical `/` remains the session's `/home/user` grant. File commands
accept `/home/user/...` and `~/...` aliases. Read commands with an explicit
`/boot/...` operand receive a separate read-only boot grant for that operation.
A writable session can copy a boot file into home with `cp /boot/file target`.
The writer receives the home grant and a separate boot handle with READ and
LOOKUP only. The Shell's cwd remains in home. `cd -` returns to the previous directory.
`hostname` changes a session-local value and `env` displays the Shell's
session values.

In a writable session, `echo text > path` replaces a file and
`echo text >> path` appends to it. The same echo formatter handles `-n`
and `-e` on the terminal and in a redirect. The writer validates and
stages the bytes before publication. Use `sync` to commit the home snapshot.
General pipelines, input redirection, redirection of other commands, command
lists, variable expansion and `source` remain part of the historical host
command language. The desktop parser reports unsupported syntax. Live memory
and disk-usage commands (mem, free, df), global ps, reboot and halt are outside
the Ring 3 command set.

`run [-s] program.snx [argument]` launches a separate Ring 3 application
and returns its PID. It accepts native files under home or read-only boot
storage. The default app receives the existing desktop home capability
subject to the target's loader restrictions. The `-s` form requests strict
policy with zero capabilities. `kill pid` is limited to processes directly
started by that Shell. F10 provides the desktop's global task manager.
`nettest` starts the trusted boot network test. It requires the writable
desktop session. The kernel checks the boot frontend's identity, the session root's
rights, and the current foreground Shell's primary client handle.
The operations use typed requests with a path and a single argument.

The ordinary image has one desktop terminal implementation. The old shared
console window, command editor and graphical welcome have been archived
outside the build tree. The kernel's emergency panic display remains
available. Explicit ShellBridge images retain the headless SCS1 command
transport and SCS2 managed execution. SELFTEST images retain the headless
command self-checks. The legacy demo and script are fixtures in those images.

Run `node "Source Code/tests/host/test-terminal-replacement.js"` against a
matching ordinary build for real keyboard, launch routes, write isolation
and snapshot checks. The Stage 2 gate also verifies
scoped process termination, directory grants and resource reclamation.
