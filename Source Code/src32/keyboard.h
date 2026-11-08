#ifndef SINO_KEYBOARD_H
#define SINO_KEYBOARD_H
/* PS/2 set 1, US physical layout. Zero means a modifier/release, not text. */
#define KEY_SHIFT_PAGE_UP 0x349
#define KEY_SHIFT_PAGE_DOWN 0x351
int keyboard_decode(unsigned char scan);
#endif
