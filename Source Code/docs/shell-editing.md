# Shell line editing

The [text console](text-console.md) starts `gsh.snx` at boot. The Shell edits
the command line and the console host draws the resulting text and caret.

## One owner of the edit line

gsh owns a bounded 255-byte UTF-8 line and a byte cursor at codepoint boundaries.
At the prompt it uses raw input without backend echo. Left/Right/Home/End,
Delete and Backspace edit codepoints; Up/Down recall the 16-entry history and
restore the unfinished draft when returning below the newest entry. Enter
commits the rendered command before parsing/executing it. Ctrl+C clears the
unfinished input and retains canceled status without killing the Shell.
Ctrl+D executes an unfinished valid line, then exits, following the EOF contract. Workers continue to use their own canonical/raw mode; foreground
restore is followed by a new raw editor frame. There is no duplicate editing
by the frontend or kernel canonical buffer.

Tab completes canonical command names and names below the explicit grant,
relative to the Shell cwd. A unique directory gets `/`, a unique file/command
gets a space, multiple matches extend the common prefix or list candidates.
UTF-8 prefixes are not cut mid-codepoint. Spaces and syntax characters in a
newly completed name are escaped. Completion applies to unquoted tokens
at the end of the line; existing quote/backslash contexts and mid-token cursors
are left unchanged instead of being rewritten incorrectly. This does not search
an ambient PATH or expose names outside the grant.

`clear` erases the current frontend's scrollback and redraws a fresh prompt.
Ctrl+L does the same while preserving the unfinished line/cursor. Shell command
history remains available. There is no ANSI escape parser or arbitrary cursor
control embedded in stdout. Other sessions are unaffected.

## Bounded repaint protocol

SYS_TERMINAL uses EDIT=17, EDIT_VIEW=18, EDIT_STATUS=19 and EDIT_ACK=20.
The 416-byte TerminalEdit contains four u32 fields (bytes, cursor, action and
serial) followed by 400 text bytes. bytes<=400, cursor<=bytes, action0 redraw/1 commit
line/2 clear history and redraw. Client sends serial0; the backend assigns a
nonzero sequence. Embedded controls/NUL and cursors inside UTF-8 continuations
are rejected. Only the foreground interactive root's primary client can submit
or query status. Only that session's controller can peek/ack. Stream-role handles
cannot submit frames, and background clients cannot clear the display.

One outstanding frame gives S_AGAIN backpressure. The controller drains earlier
stdout before accepting a frame, applies it, then acknowledges its sequence;
the Shell waits for the acknowledgment before writing command output or
starting a worker. Stale acknowledgments are rejected. Foreground changes and
cleanup reset pending edits. The frame fits in the session metadata page. `FEATURE_LINE_EDIT=1024`
advertises these operations.

The console host uses an edit anchor and a 128-row text ring. It redraws the
current preview in the 80-column model and positions the caret on the wrapped
row. The VGA display renders printable ASCII and substitutes `?` for other
characters.

## Input and verification

Accepted text and editing keys share an eight-event session queue. The console
host holds the pending key when the session reports `S_AGAIN` and retries it
before reading another key. Foreground transitions reset pending edit input.

The host tests `test-line-edit.cpp`, `test-line-model.cpp` and
`test-terminal-model.cpp` exercise the editor and display models. The
[text console checks](text-console.md#验证) cover real PS/2 input, history,
automatic session restart and scrollback.
