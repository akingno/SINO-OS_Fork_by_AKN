#ifndef SINO_TERMINAL_MODEL_H
#define SINO_TERMINAL_MODEL_H
#include "../src32/types.h"
#define TERMINAL_COLUMNS 120u
#define TERMINAL_ROWS 35u
#define TERMINAL_HISTORY 128u
#define TERMINAL_CONTINUATION 0x110000u

/* Frontend-owned history and streaming UTF-8 decoder. Non-ASCII occupies
 * three model cells; the VGA frontend renders unsupported characters as ?. */
typedef struct {
  u32 cells[TERMINAL_HISTORY][TERMINAL_COLUMNS];
  u8 wrapped[TERMINAL_HISTORY];
  u32 columns, rows, line, column, used, scroll;
  u32 pending, minimum, need;
} TerminalModel;

static void terminal_model_blank(TerminalModel *m, u32 row) {
  for (u32 i = 0; i < TERMINAL_COLUMNS; i++)
    m->cells[row][i] = ' ';
}
static void terminal_model_resize(TerminalModel *m, u32 columns, u32 rows) {
  if (columns < 4)
    columns = 4;
  if (columns > TERMINAL_COLUMNS)
    columns = TERMINAL_COLUMNS;
  if (!rows)
    rows = 1;
  if (rows > TERMINAL_ROWS)
    rows = TERMINAL_ROWS;
  m->columns = columns;
  m->rows = rows;
  if (m->column > columns)
    m->column = columns;
  u32 maximum = m->used > rows ? m->used - rows : 0;
  if (m->scroll > maximum)
    m->scroll = maximum;
}
static void terminal_model_init(TerminalModel *m, u32 columns, u32 rows) {
  u8 *bytes = (u8 *)m;
  for (u32 i = 0; i < sizeof(*m); i++)
    bytes[i] = 0;
  for (u32 i = 0; i < TERMINAL_HISTORY; i++)
    terminal_model_blank(m, i);
  m->used = 1;
  terminal_model_resize(m, columns, rows);
}
static void terminal_model_newline(TerminalModel *m, int wrapped) {
  m->line = (m->line + 1) % TERMINAL_HISTORY;
  m->column = 0;
  if (m->used < TERMINAL_HISTORY)
    m->used++;
  terminal_model_blank(m, m->line);
  if (m->scroll) {
    u32 maximum = m->used > m->rows ? m->used - m->rows : 0;
    if (m->scroll < maximum) m->scroll++;
  }
  m->wrapped[m->line] = (u8)wrapped;
}
static void terminal_model_erase(TerminalModel *m, u32 column) {
  u32 *row = m->cells[m->line];
  while (column && row[column] == TERMINAL_CONTINUATION)
    column--;
  u32 width = row[column] >= 128 ? 3 : 1;
  for (u32 i = column; i < column + width && i < TERMINAL_COLUMNS; i++)
    row[i] = ' ';
}
static void terminal_model_codepoint(TerminalModel *m, u32 cp) {
  if (cp == '\r') {
    m->column = 0;
    return;
  }
  if (cp == '\n') {
    terminal_model_newline(m, 0);
    return;
  }
  if (cp == '\b') {
    if (!m->column && m->wrapped[m->line] && m->used > 1) {
      m->line = (m->line + TERMINAL_HISTORY - 1) % TERMINAL_HISTORY;
      m->used--;
      m->column = m->columns;
    }
    if (m->column) {
      m->column--;
      while (m->column && m->cells[m->line][m->column] == TERMINAL_CONTINUATION)
        m->column--;
      terminal_model_erase(m, m->column);
    }
    return;
  }
  if (cp == '\t') {
    u32 spaces = 4 - m->column % 4;
    for (u32 i = 0; i < spaces; i++)
      terminal_model_codepoint(m, ' ');
    return;
  }
  if (cp < 32 || cp == 127)
    cp = '?';
  u32 width = cp >= 128 ? 3 : 1;
  if (m->column + width > m->columns)
    terminal_model_newline(m, 1);
  for (u32 i = 0; i < width; i++)
    terminal_model_erase(m, m->column + i);
  m->cells[m->line][m->column++] = cp;
  for (u32 i = 1; i < width; i++)
    m->cells[m->line][m->column++] = TERMINAL_CONTINUATION;
}
static void terminal_model_byte(TerminalModel *m, u8 byte) {
  if (m->need) {
    if ((byte & 0xc0) == 0x80) {
      m->pending = (m->pending << 6) | (byte & 63);
      if (!--m->need) {
        u32 cp = m->pending;
        if (cp < m->minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
          cp = '?';
        terminal_model_codepoint(m, cp);
      }
      return;
    }
    m->need = 0;
    terminal_model_codepoint(m, '?');
  }
  if (byte < 128)
    terminal_model_codepoint(m, byte);
  else if (byte >= 0xc2 && byte <= 0xdf) {
    m->pending = byte & 31;
    m->need = 1;
    m->minimum = 128;
  } else if (byte >= 0xe0 && byte <= 0xef) {
    m->pending = byte & 15;
    m->need = 2;
    m->minimum = 2048;
  } else if (byte >= 0xf0 && byte <= 0xf4) {
    m->pending = byte & 7;
    m->need = 3;
    m->minimum = 65536;
  } else
    terminal_model_codepoint(m, '?');
}
static void terminal_model_write(TerminalModel *m, const u8 *bytes, u32 count) {
  for (u32 i = 0; i < count; i++)
    terminal_model_byte(m, bytes[i]);
}
static void terminal_model_finish(TerminalModel *m) {
  if (m->need) {
    m->need = 0;
    terminal_model_codepoint(m, '?');
  }
}
static void terminal_model_scroll(TerminalModel *m, int amount) {
  int next = (int)m->scroll + amount;
  u32 maximum = m->used > m->rows ? m->used - m->rows : 0;
  if (next < 0)
    next = 0;
  m->scroll = (u32)next > maximum ? maximum : (u32)next;
}
static u32 terminal_model_row(const TerminalModel *m, u32 visible) {
  u32 shown = m->used < m->rows ? m->used : m->rows;
  return (m->line + TERMINAL_HISTORY + 1 - shown - m->scroll + visible) %
         TERMINAL_HISTORY;
}
#endif
