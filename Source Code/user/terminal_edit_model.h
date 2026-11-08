#ifndef SINO_TERMINAL_EDIT_MODEL_H
#define SINO_TERMINAL_EDIT_MODEL_H
#include "terminal_model.h"
#include "../src32/api.h"
typedef struct {
  u32 active, line, column, used, wrapped, max_rows, caret_line, caret_column;
  u32 saved[TERMINAL_COLUMNS];
} TerminalEditView;
static void terminal_edit_apply(TerminalModel *m, TerminalEditView *v,
                                const TerminalEdit *e) {
  u32 browsing = m->scroll;
  if (e->action == 2) {
    terminal_model_init(m, m->columns, m->rows);
    v->active = 0;
    browsing = 0;
  }
  if (!v->active) {
    v->line = m->line;
    v->column = m->column;
    v->used = m->used;
    v->wrapped = m->wrapped[m->line];
    v->max_rows = 0;
    for (u32 i = 0; i < TERMINAL_COLUMNS; i++)
      v->saved[i] = m->cells[m->line][i];
    v->active = 1;
  }
  m->line = v->line;
  m->column = v->column > m->columns ? m->columns : v->column;
  u32 evicted = v->used + v->max_rows > TERMINAL_HISTORY
                    ? v->used + v->max_rows - TERMINAL_HISTORY
                    : 0;
  m->used = v->used > evicted ? v->used - evicted : 1;
  m->wrapped[m->line] = (u8)v->wrapped;
  m->need = 0;
  for (u32 i = 0; i < TERMINAL_COLUMNS; i++)
    m->cells[m->line][i] = v->saved[i];
  terminal_model_write(m, (const u8 *)e->text, e->cursor);
  terminal_model_finish(m);
  v->caret_line = m->line;
  v->caret_column = m->column;
  terminal_model_write(m, (const u8 *)e->text + e->cursor,
                       e->bytes - e->cursor);
  terminal_model_finish(m);
  u32 rows = (m->line + TERMINAL_HISTORY - v->line) % TERMINAL_HISTORY;
  if (rows > v->max_rows)
    v->max_rows = rows;
  if (e->action == 1) {
    terminal_model_codepoint(m, '\n');
    v->active = 0;
  } else {
    u32 distance =
        (m->line + TERMINAL_HISTORY - v->caret_line) % TERMINAL_HISTORY;
    m->scroll = distance >= m->rows ? distance - m->rows + 1 : 0;
  }
  if (browsing) {
    u32 maximum = m->used > m->rows ? m->used - m->rows : 0;
    m->scroll = browsing > maximum ? maximum : browsing;
  }
}
#endif
