#ifndef SINO_TEXT_SCAN_H
#define SINO_TEXT_SCAN_H
#include "../src32/types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  u32 bytes, lines, words, inside;
} TextCounts;
static void text_count(TextCounts *c, u8 b) {
  c->bytes++;
  if (b == '\n')
    c->lines++;
  int space = b == ' ' || b == '\t' || b == '\n' || b == '\r';
  if (!space && !c->inside)
    c->words++;
  c->inside = !space;
}
typedef struct {
  u32 length, matched, prefix[255];
  u8 pattern[255];
} TextMatch;
static int text_pattern(TextMatch *m, const char *s, u32 size) {
  if (size > 255)
    return -1;
  m->length = size;
  m->matched = 0;
  for (u32 i = 0; i < size; i++) {
    m->pattern[i] = (u8)s[i];
    u32 n = i ? m->prefix[i - 1] : 0;
    while (n && m->pattern[i] != m->pattern[n])
      n = m->prefix[n - 1];
    if (i && m->pattern[i] == m->pattern[n])
      n++;
    m->prefix[i] = n;
  }
  return 0;
}
static int text_match(TextMatch *m, u8 b) {
  if (!m->length)
    return 1;
  while (m->matched && b != m->pattern[m->matched])
    m->matched = m->prefix[m->matched - 1];
  if (b == m->pattern[m->matched])
    m->matched++;
  if (m->matched == m->length) {
    m->matched = m->prefix[m->matched - 1];
    return 1;
  }
  return 0;
}
static int text_unsigned(const char *s, u32 *out) {
  u32 n = 0;
  if (!*s)
    return -1;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || n > (0xffffffffu - (u32)(*s - '0')) / 10)
      return -1;
    n = n * 10 + (u32)(*s - '0');
  }
  *out = n;
  return 0;
}
#ifdef __cplusplus
}
#endif
#endif
