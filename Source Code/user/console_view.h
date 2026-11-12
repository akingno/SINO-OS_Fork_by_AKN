#ifndef SINO_CONSOLE_VIEW_H
#define SINO_CONSOLE_VIEW_H
#include "terminal_edit_model.h"
#ifdef __cplusplus
extern "C" {
#endif
static void console_view_frame(const TerminalModel *m, const TerminalEditView *v,
                               ConsoleFrame *frame) {
  frame->cursor = CONSOLE_COLUMNS * CONSOLE_ROWS;
  u32 caret = v->active ? v->caret_line : m->line;
  u32 column = v->active ? v->caret_column : m->column;
  for (u32 y = 0; y < CONSOLE_ROWS; y++) {
    int pinned = m->scroll && y == CONSOLE_ROWS - 1;
    u32 row = pinned ? caret : terminal_model_row(m, y);
    for (u32 x = 0; x < CONSOLE_COLUMNS; x++) {
      u32 cp = y < m->used || pinned ? m->cells[row][x] : ' ';
      frame->cells[y * CONSOLE_COLUMNS + x] =
          cp == TERMINAL_CONTINUATION ? ' ' : cp >= 32 && cp < 127 ? cp : '?';
    }
    if ((pinned || (!m->scroll && row == caret && y < m->used)) && column < CONSOLE_COLUMNS)
      frame->cursor = y * CONSOLE_COLUMNS + column;
  }
}
#ifdef __cplusplus
}
#endif
#endif
