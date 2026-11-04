#include "locale.h"
extern int settings_language;
#include "locale_data.inc"
const char* locale_text_for(int language, TextKey key) {
  if ((u32)key >= TEXT_KEY_COUNT)
    return "[invalid text key]";
  return resources[key][language == 0 ? 0 : 1];
}
const char* locale_text(TextKey key) {
  return locale_text_for(settings_language, key);
}
static int starts(const char* s, const char* prefix) {
  while (*prefix)
    if (*s++ != *prefix++)
      return 0;
  return 1;
}
int locale_format(char* out, u32 capacity, TextKey key, const char* name,
                  u32 value) {
  if (!capacity)
    return -1;
  const char* s = locale_text(key);
  char digits[11];
  u32 n = 0, used = 0;
  do {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  } while (value);
  out[0] = 0;
  while (*s) {
    const char* part = s;
    u32 length = 1;
    if (starts(s, "{name}")) {
      s += 6;
      part = name;
      length = 0;
      while (part[length])
        length++;
    } else if (starts(s, "{count}")) {
      s += 7;
      if (n >= capacity - used) {
        out[0] = 0;
        return -1;
      }
      for (u32 j = n; j; j--)
        out[used++] = digits[j - 1];
      continue;
    } else {
      unsigned char c = (unsigned char)*s;
      length = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4;
      s += length;
    }
    if (length >= capacity - used) {
      out[0] = 0;
      return -1;
    }
    while (length--)
      out[used++] = *part++;
  }
  out[used] = 0;
  return (int)used;
}
