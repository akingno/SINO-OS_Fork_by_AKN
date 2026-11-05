#ifndef SINO_LINE_EDIT_H
#define SINO_LINE_EDIT_H
#include "../src32/types.h"
typedef struct {
  char text[256];
  u32 length, cursor;
} LineEditor;
static u32 line_previous(const LineEditor *e, u32 at) {
  if (!at)
    return 0;
  at--;
  while (at && ((u8)e->text[at] & 0xc0) == 0x80)
    at--;
  return at;
}
static u32 line_next(const LineEditor *e, u32 at) {
  if (at == e->length)
    return at;
  at++;
  while (at < e->length && ((u8)e->text[at] & 0xc0) == 0x80)
    at++;
  return at;
}
static void line_set(LineEditor *e, const char *s) {
  u32 i = 0;
  while (s[i] && i < 255) {
    e->text[i] = s[i];
    i++;
  }
  e->text[i] = 0;
  e->length = e->cursor = i;
}
static int line_insert(LineEditor *e, const char *s, u32 n) {
  if (n > 255 - e->length)
    return -1;
  for (u32 at = 0; at < n;) {
    u8 b = (u8)s[at++];
    u32 need = 0, value = 0, minimum = 0;
    if (b >= 32 && b < 127)
      continue;
    if (b >= 0xc2 && b <= 0xdf) {
      need = 1;
      value = b & 31;
      minimum = 128;
    } else if (b >= 0xe0 && b <= 0xef) {
      need = 2;
      value = b & 15;
      minimum = 2048;
    } else if (b >= 0xf0 && b <= 0xf4) {
      need = 3;
      value = b & 7;
      minimum = 65536;
    } else
      return -1;
    if (need > n - at)
      return -1;
    while (need--) {
      b = (u8)s[at++];
      if ((b & 0xc0) != 0x80)
        return -1;
      value = (value << 6) | (b & 63);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
      return -1;
  }
  for (u32 i = e->length + 1; i > e->cursor; i--)
    e->text[i + n - 1] = e->text[i - 1];
  for (u32 i = 0; i < n; i++)
    e->text[e->cursor + i] = s[i];
  e->cursor += n;
  e->length += n;
  return n ? 1 : 0;
}
static int line_key(LineEditor *e, int key) {
  u32 begin = e->cursor, end = begin;
  if (key == 331) {
    e->cursor = line_previous(e, begin);
    return begin != e->cursor;
  }
  if (key == 333) {
    e->cursor = line_next(e, begin);
    return begin != e->cursor;
  }
  if (key == 327) {
    e->cursor = 0;
    return begin != 0;
  }
  if (key == 335) {
    e->cursor = e->length;
    return begin != e->length;
  }
  if (key == 8) {
    begin = line_previous(e, begin);
    e->cursor = begin;
  } else if (key == 339)
    end = line_next(e, end);
  else
    return 0;
  if (begin == end)
    return 0;
  u32 removed = end - begin;
  for (u32 i = begin; i + removed <= e->length; i++)
    e->text[i] = e->text[i + removed];
  e->length -= removed;
  return 1;
}
#endif
