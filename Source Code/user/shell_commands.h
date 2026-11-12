#ifndef SINO_SHELL_COMMANDS_H
#define SINO_SHELL_COMMANDS_H
#include "../src32/types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  const char *name;
  u32 kind;
  const char *usage;
  const char *details;
} ShellCommand;
static const ShellCommand shell_commands[] = {
    {"run", 6, "run [-s] program.snx [argument]",
     "Start a program asynchronously; this command does not wait for its exit.\n  -s: strict mode, with no ambient file or spawn authority.\n  program.snx: /boot path or a path in your workspace.\n  argument: one optional string; quote it when it contains spaces."},
    {"kill", 6, "kill pid (applications started by this Shell)",
     "Cancel an application started by this Shell.\n  pid: positive process ID printed by run."},
    {"nettest", 6, "nettest",
     "Start the loopback network test (127.0.0.1).\n  No arguments; requires a writable session."},
    {"whoami", 0, "whoami",
     "Print the current user name. No arguments."},
    {"hostname", 0, "hostname [name]",
     "Print or change this Shell session's host name.\n  name: 1..31 letters, digits or hyphens; omit to print it."},
    {"env", 0, "env",
     "Print this Shell session's environment information.\n  No arguments; this command does not set variables."},
    {"sleep", 1, "sleep seconds (0..3600)",
     "Wait for the requested time; Ctrl+C cancels the task.\n  seconds: integer from 0 to 3600."},
 {"clear", 0, "clear",
     "Clear the display history and redraw the prompt.\n  No arguments. Shortcut: Ctrl+L."},
    {"touch", 5, "touch path",
     "Create an empty file if absent; keep existing contents.\n  path: workspace file path. Requires a writable session."},
    {"mkdir", 5, "mkdir path",
     "Create one directory.\n  path: new directory; its parent must exist. No -p option."},
    {"rm", 5, "rm path",
     "Remove one file.\n  path: existing workspace file. No recursive removal."},
    {"rmdir", 5, "rmdir path",
     "Remove one empty directory.\n  path: existing workspace directory; leave it with cd first."},
    {"write", 5, "write path text...",
     "Replace a file with the supplied text and a final newline.\n  path: destination file. text...: arguments joined with spaces.\n  Quote text containing spaces. Requires a writable session."},
    {"append", 5, "append path text...",
     "Append text and a final newline, creating the file if absent.\n  path: destination file. text...: arguments joined with spaces.\n  Requires a writable session."},
    {"cp", 5, "cp source target",
     "Copy file contents. Requires a writable session.\n  source: workspace file, or a read-only /boot file.\n  target: file path or existing directory; may replace a file."},
    {"mv", 5, "mv source target",
     "Rename or move a workspace file or directory.\n  source: existing path. target: new path or existing directory.\n  An existing destination file is not overwritten."},
    {"sync", 5, "sync",
     "Save the workspace snapshot to disk. No arguments.\n  Successful write commands already save automatically.\n  Use sync to retry after a save failure."},
    {"help", 0, "help [command]",
     "List all recognized commands by category.\n  command: optional name or alias for purpose and parameter details.\n  [] means optional; ... means more arguments."},
    {"pwd", 0, "pwd",
     "Print this Shell's current workspace directory. No arguments."},
    {"cd", 0, "cd [path]",
     "Change the current workspace directory.\n  path: relative or absolute workspace path; omitted means root.\n  ..: parent directory. -: previous directory. / maps to /home/user."},
    {"history", 0, "history",
     "Print up to 16 recent nonempty command lines.\n  No arguments; Up/Down recall previous input."},
    {"status", 0, "status",
     "Show the result retained from the previous command.\n  code: exit/result code; 0 usually means success.\n  reason: 0=no worker result, 1=exited, 2=faulted,\n          3=canceled, 4=killed.\n  start: 0=no startup error; negative values explain rejection.\n  status itself preserves this result; builtins can update it."},
    {"exit", 0, "exit [0..255]",
     "Exit the Shell.\n  code: optional integer 0..255; omitted uses the retained code.\n  The text console starts a fresh Shell after exit."},
    {"which", 0, "which command",
     "Show how a recognized command is provided.\n  command: name or alias; no global PATH search is performed."},
    {"echo", 1, "echo [-n] [-e] [text...]",
     "Print text arguments separated by spaces.\n  -n: do not append a newline.\n  -e: interpret \\n (newline), \\t (tab) and \\\\ (backslash).\n  Options may be combined as separate arguments: -n -e, not -ne.\n  Use single quotes to preserve backslashes through Shell parsing.\n  Example: echo -e 'first\\nsecond'\n  > path: replace a file; >> path: append (writable session)."},
    {"date", 1, "date",
     "Print the RTC calendar date as YYYY-MM-DD. No arguments."},
    {"time", 1, "time",
     "Print the RTC time as HH:MM:SS. No arguments."},
    {"uptime", 1, "uptime",
     "Print seconds elapsed since boot. No arguments."},
    {"uname", 1, "uname [-a]",
     "Print the system name.\n  -a: include the version and i386 architecture."},
    {"ver", 1, "ver",
     "Print the system version and architecture. No arguments."},
    {"about", 1, "about",
     "Print system and source attribution information. No arguments."},
    {"credits", 1, "credits",
     "Print project, toolchain and firmware credits. No arguments."},
    {"ls", 2, "ls [-a|-l|-la|-al] [path]",
     "List entries in a directory; omitted path means current directory.\n  -a: include names beginning with a dot.\n  -l: print kind (f=file, d=directory) and byte length.\n  -la or -al: combine both options."},
    {"stat", 2, "stat path",
     "Print file kind, byte length and revision identifier.\n  path: file or directory. Revision is not a per-file edit count."},
    {"cat", 2, "cat path",
     "Display a file as safe text without pagination.\n  path: workspace or read-only /boot file."},
    {"view", 2, "view path",
     "Display a file one page at a time.\n  path: file to read. Space: next page. q: stop viewing."},
    {"head", 2, "head [-n N] path",
     "Display the first lines of a file.\n  -n N: nonnegative line count; default 10. N=0 prints nothing.\n  path: file to read."},
    {"tail", 2, "tail [-n N] path",
     "Display the last lines of a file.\n  -n N: nonnegative line count; default 10. N=0 prints nothing.\n  path: file to read."},
    {"wc", 2, "wc [-l|-w|-c] path",
     "Count original file bytes, before display sanitizing.\n  -l: newline count. -w: ASCII-whitespace word count.\n  -c: byte count. Omit the option to print all three.\n  path: file to read."},
    {"grep", 2, "grep [-n] text path",
     "Print lines containing literal text; matching is case-sensitive.\n  -n: prefix each matching line with its 1-based line number.\n  text: substring, not a regular expression. path: file to search.\n  No matching lines returns code 1."},
    {"hexdump", 2, "hexdump path",
     "Display file offsets, hexadecimal bytes and safe ASCII.\n  path: file to read; output uses 16 bytes per row."},
    {"tree", 2, "tree [path]",
     "List logical paths recursively, starting at path.\n  path: optional file/directory; omitted means current directory."},
    {"find", 2, "find [path] [text]",
     "List paths recursively, optionally filtering their names.\n  path: optional starting path; omitted means current directory.\n  text: optional literal substring of the path, not file contents."},
    {"args", 3, "args [text...]",
     "Print argc, and the byte length and value of every argument.\n  text...: optional arguments; argv[0] is the command name.\n  Example: args \"\" \"a b\""},
    {"exitcode", 3, "exitcode signed-i32",
     "Return the requested signed 32-bit exit code.\n  signed-i32: integer from -2147483648 to 2147483647.\n  Run status next to inspect the retained result."},
    {"spin", 3, "spin",
     "Wait until canceled with Ctrl+C. No arguments."},
    {"fault", 3, "fault",
     "Trigger an intentional invalid-instruction fault in the worker.\n  No arguments; the Shell should remain available afterward."},
    {"mem", 4, "mem: needs a public live-memory query",
     "Unavailable: the public interface has no live-memory query."},
    {"free", 4, "free: needs a public live-memory query",
     "Unavailable: the public interface has no live-memory query."},
    {"df", 4, "df: needs a public storage query",
     "Unavailable: the public interface has no storage-usage query."},
    {"ps", 4, "ps: needs an authorized process query",
     "Unavailable: the public interface has no global process query."},
};
static int shell_equal(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}
static const char *shell_alias(const char *s) {
  const char *from[] = {"DIR", "dir", "TYPE", "type", "VER", "LS", "CAT",
                        "cls", "md", "del", "copy", "ren", 0};
  const char *to[] = {"ls", "ls", "cat", "cat", "ver", "ls", "cat",
                      "clear", "mkdir", "rm", "cp", "mv"};
  for (u32 i = 0; from[i]; i++)
    if (shell_equal(s, from[i]))
      return to[i];
  return s;
}
static const ShellCommand *shell_command(const char *s) {
  s = shell_alias(s);
  for (u32 i = 0; i < sizeof(shell_commands) / sizeof(*shell_commands); i++)
    if (shell_equal(s, shell_commands[i].name))
      return shell_commands + i;
  return 0;
}
#ifdef __cplusplus
}
#endif
#endif
