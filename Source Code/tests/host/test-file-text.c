/* Compile the same streaming decoder/layout used by the strict file tools. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../user/file_text.h"
typedef struct {
  const unsigned char *p;
  unsigned size, pos;
} Input;
static int next(void *context) {
  Input *in = context;
  return in->pos == in->size ? -1 : in->p[in->pos++];
}
static void decode(const unsigned char *data, unsigned bytes,
                   const unsigned *expected, unsigned count) {
  Input in = {data, bytes, 0};
  FileText state = {};
  unsigned cp;
  for (unsigned i = 0; i < count; i++) {
    assert(file_text_next(&state, next, &in, &cp) == 1);
    assert(cp == expected[i]);
  }
  assert(file_text_next(&state, next, &in, &cp) == 0);
}
int main(void) {
  const unsigned char bytes[] = {'A',  0,    27,   8,    127,  13,   9,    10,
                                 0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80, 0xe4,
                                 'Z',  0xc0, 0xed, 0xa0, 0x80, 0xe4, 0xbd};
  const unsigned expected[] = {'A',    '?',     '?', '?', '?', '?', 9,  10,
                               0x4f60, 0x1f600, '?', 'Z', '?', '?', '?'};
  decode(bytes, sizeof(bytes), expected, sizeof(expected) / sizeof(*expected));
  decode(bytes, 0, expected, 0);
  unsigned char split[515];
  memset(split, 'x', 511);
  split[511] = 0xe4;
  split[512] = 0xbd;
  split[513] = 0xa0;
  split[514] = 'z';
  Input in = {split, sizeof(split), 0};
  FileText state = {};
  unsigned cp;
  for (unsigned i = 0; i < 511; i++)
    assert(file_text_next(&state, next, &in, &cp) == 1 && cp == 'x');
  assert(file_text_next(&state, next, &in, &cp) == 1 && cp == 0x4f60);
  assert(file_text_next(&state, next, &in, &cp) == 1 && cp == 'z');
  char utf[4];
  assert(file_text_encode(0x4f60, utf) == 3 && !memcmp(utf, "\xe4\xbd\xa0", 3));
  assert(file_text_encode(0x1f600, utf) == 4);
  FileLayout layout = {};
  assert(!file_text_wrap(&layout, 80, 'x'));
  layout.column = 80;
  assert(file_text_wrap(&layout, 80, 'x'));
  layout.column = 78;
  assert(file_text_wrap(&layout, 80, 0x4f60));
  assert(!file_text_wrap(&layout, 80, '\n'));
  puts("FILE TEXT PASS");
  return 0;
}
