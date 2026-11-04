#ifndef SINO_LOCALE_H
#define SINO_LOCALE_H
#include "types.h"
#include "locale_keys.h"
const char* locale_text(TextKey);
const char* locale_text_for(int language, TextKey);
/* Bounded templates support {name} and {count}; returns -1 on overflow. */
int locale_format(char*, u32, TextKey, const char*, u32);
#endif
