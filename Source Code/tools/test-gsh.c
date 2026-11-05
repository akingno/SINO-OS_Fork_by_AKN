#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../user/gsh_parse.h"
int main(void) {
  ShellWords w;
  const char *input = "echo '' \"a b\" c\\ d 你好";
  assert(shell_parse(input, (u32)strlen(input), &w) == 0 && w.argc == 5);
  assert(!strcmp(w.bytes + w.offsets[1], ""));
  assert(!strcmp(w.bytes + w.offsets[2], "a b"));
  assert(!strcmp(w.bytes + w.offsets[3], "c d"));
  assert(!strcmp(w.bytes + w.offsets[4], "你好"));
  assert(shell_parse("echo 'x", 7, &w) < 0);
  assert(shell_parse("echo | cat", 10, &w) < 0);
  const char trailing_slash[] = {'e', 'c', 'h', 'o', ' ', '\\', 0};
  assert(shell_parse(trailing_slash, 6, &w) < 0);
  assert(shell_parse("echo '*|$'", 10, &w) == 0);
  assert(shell_parse("a\0b", 3, &w) < 0);
  assert(shell_parse(" \t ", 3, &w) == 0 && w.argc == 0);
  assert(shell_parse("a a a a a a a a a a a a a a a a a", 33, &w) < 0);
  assert(shell_parse("echo a\"b\"c", 10, &w) == 0);
  assert(!strcmp(w.bytes + w.offsets[1], "abc"));
  char cwd[128];
  assert(shell_path("a/b", "../c/./", cwd) == 0 && !strcmp(cwd, "a/c"));
  assert(shell_path("a/b", "/", cwd) == 0 && !strcmp(cwd, ""));
  assert(shell_path("", "..", cwd) < 0);
  assert(shell_path("a", "../../x", cwd) < 0);
  assert(shell_path("a", "你好", cwd) == 0 && !strcmp(cwd, "a/你好"));
  i32 n;
  assert(shell_integer("-2147483648", &n) == 0 && n == (-2147483647 - 1));
  assert(shell_integer("2147483648", &n) < 0);
  assert(shell_integer("1x", &n) < 0);
  assert(shell_integer("", &n) < 0);
  char maximum[256];
  memset(maximum, 'a', 255);
  maximum[255] = 0;
  assert(shell_parse(maximum, 255, &w) == 0 && w.used == 256);
  assert(shell_path("", maximum, cwd) < 0);
  assert(shell_integer("2147483647", &n) == 0 && n == 2147483647);
  assert(shell_integer("-2147483649", &n) < 0);
  puts("GSH PARSER PASS");
  return 0;
}
