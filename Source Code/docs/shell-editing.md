# Shell line editing and formal terminal entries

F2, the SINO menu and Terminal.app open writable **SINO Shell**. F12 opens
a read-only session. A writable Shell can open another session with
`run /boot/term.snx` or `run /boot/term.snx /write`. See
[desktop-shell.md](desktop-shell.md) for routes and authority.

## One owner of the edit line

gsh owns a bounded255-byte UTF-8 line and a byte cursor at codepoint boundaries.
At the prompt it uses raw input without backend echo. Left/Right/Home/End,
Delete and Backspace edit codepoints; Up/Down recall the16-entry history and
restore the unfinished draft when returning below the newest entry. Enter
commits the rendered command before parsing/executing it. Ctrl+C clears the
unfinished input and retains canceled status without killing the Shell.
Ctrl+D executes an unfinished valid line, then exits, following the earlier
EOF contract. Workers continue to use their own canonical/raw mode; foreground
restore is followed by a new raw editor frame. There is no duplicate editing
by the frontend or kernel canonical buffer.

Tab completes canonical command names and names below the explicit grant,
relative to the Shell cwd. A unique directory gets `/`, a unique file/command
gets a space, multiple matches extend the common prefix or list candidates.
UTF-8 prefixes are not cut mid-codepoint. Spaces and syntax characters in a
newly completed name are escaped. The first version completes unquoted tokens
at the end of the line; existing quote/backslash contexts and mid-token cursors
are left unchanged instead of being rewritten incorrectly. This does not search
an ambient PATH or expose names outside the grant.

`clear` erases the current frontend's scrollback and redraws a fresh prompt.
Ctrl+L does the same while preserving the unfinished line/cursor. Shell command
history remains available. There is no ANSI escape parser or arbitrary cursor
control embedded in stdout. Other sessions are unaffected.

## Bounded repaint protocol

SYS_TERMINAL appends operations EDIT=17, EDIT_VIEW=18, EDIT_STATUS=19 and
EDIT_ACK=20. TerminalEdit is416 bytes: bytes, cursor, action, serial (four u32),
followed by400 text bytes. bytes<=400, cursor<=bytes, action0 redraw/1 commit
line/2 clear history and redraw. Client sends serial0; the backend assigns a
nonzero sequence. Embedded controls/NUL and cursors inside UTF-8 continuations
are rejected. Only the foreground interactive root's primary client can submit
or query status. Only that session's controller can peek/ack. Stream-role handles
cannot submit frames, and background clients cannot clear the display.

One outstanding frame gives S_AGAIN backpressure. The controller drains earlier
stdout before accepting a frame, applies it, then acknowledges its sequence;
the Shell waits for the acknowledgment before writing command output or
starting a worker. Stale acknowledgments are rejected. Foreground changes and
cleanup reset pending edits. The frame fits the existing single metadata page;
no new retained-run or capture slots are used. FEATURE_LINE_EDIT=1024 makes
the current feature mask2047; earlier API layouts and operation numbers remain.

The frontend uses a small edit anchor plus the existing text ring. Repainting
rolls back the prior preview and rebuilds it using the same one-cell ASCII and
three-cell non-ASCII rules; the caret follows its codepoint position across
wrapped rows. Preview rows may evict old scrollback under the existing128-row
bound; they are not stored as duplicate commands. Resize reflows the live edit
through a new frame. This is a text model, not a general terminal emulator.

## Input pressure and paste

While a root is editing, accepted text bytes and typed keys share the same
eight-event FIFO. A UTF-8 feed is accepted atomically or retried; text cannot be
overtaken by arrow keys. The frontend's64-item queue retains accepted entries
across S_AGAIN. Overflow cancels unfinished input with explicit feedback rather
than executing a silently truncated line. Already completed commands are not
rolled back. Root Ctrl+C can clear the pending edit-input FIFO; ordinary F/G2
clients keep their earlier cancellation contract.

For trusted terminal frontends, the15-slot GUI delivery queue has independent
close/cancel delivery and a saturating lost-input notice. Overflow causes queued
unfinished input to be rejected as a unit before it is forwarded. Other GUI
applications retain their existing queue behavior. These are bounded rejection
guarantees, not unlimited lossless input or hard real-time promises.

Ctrl+V at a Shell prompt snapshots at most255 clipboard bytes in the compatible
frontend. Invalid UTF-8, controls/newlines, an oversized clipboard, or a paste
attempt while a worker runs are rejected. Valid single-line content is queued
in whole codepoints in bounded turns with backpressure; Enter is never pasted
automatically. New text typed during the paste is explicitly rejected, while
cancel and close remain available. The strict Shell still cannot read the
ambient clipboard. Appending paste beyond the remaining line capacity rejects
the unfinished line, not a truncated executable command.

## Diagnostic separation

The formal frontend no longer shares its boot image with the bounded probe.
termqa.snx holds terminal diagnostics; brokerqa.snx replaces the old g2test name.
They are packaged only in explicit ShellBridge test images. Ordinary ISO entry
points no longer offer phase-number flags or launch a demo probe. Numeric ABI
catalog slots for diagnostic tests remain stable; historical stage labels in
saved evidence are not rewritten or deleted.

Host tests compile the actual line editor, repaint model and input queue.
Real QMP scenarios exercise the formal F12 entry and integration with the
existing read/write tools. Run stage2-test.ps1 and inspect an explicit saved
run using stage2-summary.ps1. Full ANSI/PTY, grapheme-cluster editing, multiline
paste retain separate scopes.
