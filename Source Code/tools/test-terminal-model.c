/* Hosted assertions against the frontend's actual text model. */
#include <stdio.h>
#include "../user/terminal_model.h"
static TerminalModel other_model;
static int terminal_model_selftest(TerminalModel *m) {
#define REQUIRE_MODEL(test) \
  do {                      \
    if (!(test))            \
      return __LINE__;      \
  } while (0)
  terminal_model_init(m, 8, 3);
  terminal_model_write(m, (const u8 *)"abc\rX\tZ", 7);
  REQUIRE_MODEL(m->cells[0][0] == 'X' && m->cells[0][4] == 'Z');
  terminal_model_write(m, (const u8 *)"\n\xe4\xbd", 3);
  REQUIRE_MODEL(m->need == 1 && m->column == 0);
  terminal_model_write(m, (const u8 *)"\xa0\xe5\xa5\xbd", 4);
  REQUIRE_MODEL(m->cells[1][0] == 0x4f60 && m->cells[1][3] == 0x597d &&
                m->column == 6);
  terminal_model_write(m, (const u8 *)"\b!", 2);
  REQUIRE_MODEL(m->column == 4 && m->cells[1][3] == '!' &&
                m->cells[1][4] == ' ');
  terminal_model_write(m, (const u8 *)"\rQ", 2);
  REQUIRE_MODEL(m->cells[1][0] == 'Q' && m->cells[1][1] == ' ' &&
                m->cells[1][2] == ' ');
  const u8 bad[] = {0, 'X', 27, 'Y', 0xe0, 0x80, 0x80, 0xed, 0xa0, 0x80};
  terminal_model_init(m, 40, 3);
  terminal_model_write(m, bad, sizeof(bad));
  REQUIRE_MODEL(m->cells[0][0] == '?' && m->cells[0][1] == 'X' &&
                m->cells[0][2] == '?' && m->cells[0][3] == 'Y' &&
                m->cells[0][4] == '?' && m->cells[0][5] == '?');
  terminal_model_write(m, (const u8 *)"\xf4\x90\x80\x80\xc2Z\xf0\x9f", 9);
  terminal_model_finish(m);
  REQUIRE_MODEL(m->cells[0][6] == '?' && m->cells[0][7] == '?' &&
                m->cells[0][8] == 'Z' && m->cells[0][9] == '?');
  terminal_model_init(m, 4, 2);
  terminal_model_write(m, (const u8 *)"1234\nA", 6);
  REQUIRE_MODEL(m->used == 2 && m->cells[1][0] == 'A');
  terminal_model_init(m, 4, 2);
  terminal_model_write(m, (const u8 *)"12345\b\b", 7);
  REQUIRE_MODEL(m->line == 0 && m->column == 3 && m->cells[0][3] == ' ');
  for (int i = 0; i < 300; i++)
    terminal_model_write(m, (const u8 *)"x\n", 2);
  REQUIRE_MODEL(m->used == TERMINAL_HISTORY && m->line < TERMINAL_HISTORY);
  terminal_model_scroll(m, 1000);
  REQUIRE_MODEL(m->scroll == TERMINAL_HISTORY - 2);
  terminal_model_scroll(m, -1000);
  REQUIRE_MODEL(m->scroll == 0);
  terminal_model_init(&other_model, 4, 2);
  REQUIRE_MODEL(other_model.used == 1 && other_model.cells[0][0] == ' ' &&
                m->used > 1);
  terminal_model_resize(m, 80, 25);
  REQUIRE_MODEL(m->columns == 80 && m->rows == 25 && m->column <= 80);
  terminal_model_init(m, 80, 25);
  for (int i = 0; i < 26; i++)
    terminal_model_write(m, (const u8 *)"\xe4\xbd\xa0", 3);
  char encoded[80];
  u32 next;
  int n = terminal_model_segment(m, 0, 0, encoded, &next);
  REQUIRE_MODEL(n == 78 && next == 80 && encoded[n] == 0);
#undef REQUIRE_MODEL
  return 0;
}

static TerminalModel model;
int main(void) {
  int failed = terminal_model_selftest(&model);
  if (failed) {
    printf("TERMINAL MODEL FAIL line=%d\n", failed);
    return 1;
  }
  puts("TERMINAL MODEL PASS");
  return 0;
}
