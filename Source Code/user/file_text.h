#ifndef SINO_FILE_TEXT_H
#define SINO_FILE_TEXT_H
#include "../src32/types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  int saved, has_saved;
} FileText;
typedef struct {
  u32 column, lines;
} FileLayout;
/* get returns a byte, -1 for EOF, or an error below -1. Decoder state spans
 * read buffers; malformed sequences and terminal controls become visible '?'.
 */
static int file_text_next(FileText *s, int (*get)(void *), void *context,
                          u32 *cp) {
  int b;
  if (s->has_saved) {
    b = s->saved;
    s->has_saved = 0;
  } else
    b = get(context);
  if (b < 0)
    return b == -1 ? 0 : b;
  if (b < 128) {
    *cp = (b < 32 && b != '\n' && b != '\t') || b == 127 ? '?' : (u32)b;
    return 1;
  }
  u32 value, minimum, need;
  if (b >= 0xc2 && b <= 0xdf) {
    value = (u32)b & 31;
    minimum = 128;
    need = 1;
  } else if (b >= 0xe0 && b <= 0xef) {
    value = (u32)b & 15;
    minimum = 2048;
    need = 2;
  } else if (b >= 0xf0 && b <= 0xf4) {
    value = (u32)b & 7;
    minimum = 65536;
    need = 3;
  } else {
    *cp = '?';
    return 1;
  }
  while (need--) {
    b = get(context);
    if (b < -1)
      return b;
    if (b == -1) {
      *cp = '?';
      return 1;
    }
    if ((b & 0xc0) != 0x80) {
      s->saved = b;
      s->has_saved = 1;
      *cp = '?';
      return 1;
    }
    value = (value << 6) | ((u32)b & 63);
  }
  *cp = value < minimum || value > 0x10ffff ||
                (value >= 0xd800 && value <= 0xdfff)
            ? '?'
            : value;
  return 1;
}
static u32 file_text_encode(u32 cp, char out[4]) {
  if (cp < 128) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 2048) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 63));
    return 2;
  }
  if (cp < 65536) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 63));
    out[2] = (char)(0x80 | (cp & 63));
    return 3;
  }
  out[0] = (char)(0xf0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 63));
  out[2] = (char)(0x80 | ((cp >> 6) & 63));
  out[3] = (char)(0x80 | (cp & 63));
  return 4;
}
static int file_text_wrap(const FileLayout *s, u32 columns, u32 cp) {
  return cp != '\n' && s->column + (cp >= 128 ? 3u : 1u) > columns;
}
#ifdef __cplusplus
}
#endif
#endif
