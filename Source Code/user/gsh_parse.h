#ifndef SINO_GSH_PARSE_H
#define SINO_GSH_PARSE_H
#include "../src32/types.h"
#define SHELL_LINE 256u
typedef struct {
  u32 argc, used, offsets[16];
  char bytes[1024];
} ShellWords;
/* Explicit input length rejects embedded NUL. Quotes concatenate, and an empty
 * quoted word still has an argv slot. Failed parses never execute a prefix. */
static int shell_parse(const char *input, u32 length, ShellWords *words) {
  words->argc = words->used = 0;
  if (length >= SHELL_LINE)
    return -1;
  u32 pos = 0;
  while (pos < length) {
    while (pos < length && (input[pos] == ' ' || input[pos] == '\t'))
      pos++;
    if (pos == length)
      break;
    if (words->argc == 16)
      return -1;
    words->offsets[words->argc++] = words->used;
    char quote = 0;
    while (pos < length) {
      char c = input[pos++];
      if (!quote && (c == ' ' || c == '\t'))
        break;
      if (!c || ((u8)c < 32 && c != '\t'))
        return -1;
      if (c == '\\' && quote != '\'') {
        if (pos == length)
          return -1;
        c = input[pos++];
        if (!c || (u8)c < 32)
          return -1;
      } else if (c == '\'' || c == '"') {
        if (!quote) {
          quote = c;
          continue;
        }
        if (quote == c) {
          quote = 0;
          continue;
        }
      } else if (!quote &&
                 (c == '|' || c == '&' || c == ';' || c == '<' || c == '>' ||
                  c == '$' || c == '`' || c == '*' || c == '?'))
        return -1;
      if (words->used >= sizeof(words->bytes) - 1)
        return -1;
      words->bytes[words->used++] = c;
    }
    if (quote)
      return -1;
    words->bytes[words->used++] = 0;
  }
  return 0;
}
/* Normalize within an explicit grant root. Absolute syntax denotes that root.
 */
static int shell_path(const char *cwd, const char *path, char out[128]) {
  u32 used = 0, pos = 0;
  if (*path != '/')
    while (cwd[used]) {
      if (used == 127)
        return -1;
      out[used] = cwd[used];
      used++;
    }
  while (path[pos]) {
    while (path[pos] == '/')
      pos++;
    if (!path[pos])
      break;
    u32 start = pos;
    while (path[pos] && path[pos] != '/')
      pos++;
    u32 length = pos - start;
    if (length == 1 && path[start] == '.')
      continue;
    if (length == 2 && path[start] == '.' && path[start + 1] == '.') {
      if (!used)
        return -1;
      while (used && out[used - 1] != '/')
        used--;
      if (used)
        used--;
      continue;
    }
    if (used + (used != 0) + length >= 128)
      return -1;
    if (used)
      out[used++] = '/';
    for (u32 i = 0; i < length; i++)
      out[used++] = path[start + i];
  }
  out[used] = 0;
  return 0;
}
static int shell_integer(const char *text, i32 *out) {
  u32 value = 0, limit = 2147483647u;
  int negative = *text == '-';
  if (negative) {
    text++;
    limit++;
  }
  if (!*text)
    return -1;
  for (; *text; text++) {
    if (*text < '0' || *text > '9')
      return -1;
    u32 digit = (u32)(*text - '0');
    if (value > (limit - digit) / 10)
      return -1;
    value = value * 10 + digit;
  }
  *out = (i32)(negative ? 0u - value : value);
  return 0;
}
#endif
