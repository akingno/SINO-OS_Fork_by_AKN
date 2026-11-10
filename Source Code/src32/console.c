#include "klog.h"
#include "console.h"
#include "kernel.h"
#include "lib/kstring.h"
#include "mm/addr.h"
#define SCROLL_LINES CON_ROWS
/* VGA text mode: 80x25 cells of one character byte and one attribute byte. */
#define VGA_TEXT_BASE ((phys_t)0xb8000u)
static u32 cells[SCROLL_LINES][CON_COLS];
static u32 utf_pending;
static int utf_need;
static u32 line, column;
static void blank(u32 n) {
  for (u32 i = 0; i < CON_COLS; i++)
    cells[n % SCROLL_LINES][i] = ' ';
}
static void cursor(int pos) {
  outb(0x3d4, 0x0a);
  u8 v = inb(0x3d5);
  outb(0x3d5, pos < 0 ? (v | 0x20) : (v & ~0x20));
  if (pos >= 0) {
    outb(0x3d4, 0x0e);
    outb(0x3d5, (u8)(pos >> 8));
    outb(0x3d4, 0x0f);
    outb(0x3d5, (u8)pos);
  }
}
void con_init(void) {
  con_clear();
  outb(0x3d4, 0x0a);
  outb(0x3d5, 14);
  outb(0x3d4, 0x0b);
  outb(0x3d5, 15);
}
void console_present(const u16* frame, u32 position) {
  volatile u16* video = phys_to_virt(VGA_TEXT_BASE);
  for (u32 i = 0; i < CON_COLS * CON_ROWS; i++) video[i] = 0x0700u | (frame[i] & 0xffu);
  cursor(position < CON_COLS * CON_ROWS ? (int)position : -1);
}
void con_clear(void) {
  line = column = utf_pending = 0;
  utf_need = 0;
  for (u32 i = 0; i < SCROLL_LINES; i++)
    blank(i);
  con_flush();
}
static void put_codepoint(u32 c) {
  if (c == '\r') {
    column = 0;
    return;
  }
  if (c == '\n') {
    line++;
    column = 0;
    blank(line);
    return;
  }
  if (c == '\t') {
    do {
      con_put(' ');
    } while (column & 3);
    return;
  }
  if (c == '\b') {
    if (column)
      cells[line % SCROLL_LINES][--column] = ' ';
    return;
  }
  if (c < 32)
    c = '?';
  int width = c >= 128 ? 3 : 1;
  if (column + (u32)width > CON_COLS) {
    line++;
    column = 0;
    blank(line);
  }
  cells[line % SCROLL_LINES][column++] = c;
  for (int i = 1; i < width; i++)
    cells[line % SCROLL_LINES][column++] = 0x110000;
  if (column == CON_COLS) {
    column = 0;
    line++;
    blank(line);
  }
}
void con_put(char c) {
  u8 b = (u8)c;
  if (utf_need) {
    if ((b & 0xc0) == 0x80) {
      utf_pending = (utf_pending << 6) | (b & 63);
      if (!--utf_need) {
        if (utf_pending < 128 || utf_pending > 0x10ffff ||
            (utf_pending >= 0xd800 && utf_pending <= 0xdfff))
          utf_pending = '?';
        put_codepoint(utf_pending);
      }
      return;
    }
    utf_need = 0;
    put_codepoint('?');
  }
  if (b < 128) {
    put_codepoint(b);
    return;
  }
  if (b >= 0xc2 && b <= 0xdf) {
    utf_pending = b & 31;
    utf_need = 1;
  } else if (b >= 0xe0 && b <= 0xef) {
    utf_pending = b & 15;
    utf_need = 2;
  } else if (b >= 0xf0 && b <= 0xf4) {
    utf_pending = b & 7;
    utf_need = 3;
  } else
    put_codepoint('?');
}
void con_write(const char* s) {
  pr_debug(s);
  while (*s)
    con_put(*s++);
}
void con_flush(void) {
  u32 start = line >= CON_ROWS ? line - CON_ROWS + 1 : 0;
  volatile u16* v = phys_to_virt(VGA_TEXT_BASE);
  for (u32 y = 0; y < CON_ROWS; y++)
    for (u32 x = 0; x < CON_COLS; x++) {
      u32 c =
          start + y <= line ? cells[(start + y) % SCROLL_LINES][x] : ' ';
      v[y * CON_COLS + x] =
          (u16)(0x0700u | (c < 128 ? c : '?'));
    }
  u32 p = line * CON_COLS + column;
  cursor(p < start * CON_COLS ||
                 p >= (start + CON_ROWS) * CON_COLS
             ? -1
             : (int)(p - start * CON_COLS));
}
void console_panic(const char* s, u32 vector, u32 error) {
  con_clear();
  con_write("SINO kernel panic\n");
  con_write(s);
  con_put('\n');
  char b[11];
  number(vector, b);
  con_write("Vector: ");
  con_write(b);
  number(error, b);
  con_write("  Error: ");
  con_write(b);
  con_write("\nCPU halted; reset the virtual machine.\n");
  con_flush();
}
