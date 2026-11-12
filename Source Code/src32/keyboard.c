#include "keyboard.h"
static unsigned shift, control, alt, locks_down;
static int extended, pause_bytes, caps, num = 1;
static const char plain[128] = {
    0,  0, '1','2','3','4','5','6','7','8','9','0','-','=', 0,  0,
   'q','w','e','r','t','y','u','i','o','p','[',']', 0,  0, 'a','s',
   'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
   'b','n','m',',','.',  '/', 0, '*', 0, ' ', 0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0, '-', 0,  0,  0, '+', 0,
    0,  0,  0,  0,  0,  0,'\\'};
int keyboard_decode(unsigned char scan) {
  if (pause_bytes) {
    pause_bytes--;
    return 0;
  }
  if (scan == 0xe1) {
    pause_bytes = 5;
    extended = 0;
    return 0;
  }
  if (scan == 0xe0) {
    extended = 1;
    return 0;
  }
  int ext = extended, up = scan & 128;
  unsigned code = scan & 127, bit = ext ? 2u : 1u;
  extended = 0;
  /* Print Screen's E0 2A/E0 AA are fake shifts. They must never release a held
   * Shift. */
  if ((code == 42 || code == 54) && !ext) {
    bit = code == 42 ? 1u : 2u;
    if (up)
      shift &= ~bit;
    else
      shift |= bit;
    return 0;
  }
  if (code == 29) {
    if (up)
      control &= ~bit;
    else
      control |= bit;
    return 0;
  }
  if (code == 56) {
    if (up)
      alt &= ~bit;
    else
      alt |= bit;
    return 0;
  }
  if (ext) {
    if (up)
      return 0;
    if (code == 28)
      return 13;
    if (code == 53)
      return '/';
    if (shift && (code == 73 || code == 81))
      return 0x300 | (int)code;
    if (code >= 71 && code <= 83)
      return 0x100 | (int)code;
    return 0;
  }
  if (code == 58 || code == 69) {
    bit = code == 58 ? 1u : 2u;
    if (up)
      locks_down &= ~bit;
    else if (!(locks_down & bit)) {
      locks_down |= bit;
      if (code == 58)
        caps = !caps;
      else
        num = !num;
    }
    return 0;
  }
  if (up)
    return 0;
  if (code == 1)
    return 27;
  if (code == 28)
    return 13;
  if (code == 14)
    return 8;
  if (code == 15)
    return alt ? 0x20f : 9;
  if ((code >= 59 && code <= 68) || code == 87 || code == 88)
    return 0x100 | (int)code;
  if (code >= 71 && code <= 83 && code != 74 && code != 78) {
    static const char keypad[] = "789-456+1230.";
    if (num ^ !!shift)
      return keypad[code - 71];
    return (shift && (code == 73 || code == 81) ? 0x300 : 0x100) | (int)code;
  }
  char c = plain[code];
  if (control && c == ' ')
    return 0x220;
  if (control && c >= 'a' && c <= 'z')
    return c - 'a' + 1;
  if (c >= 'a' && c <= 'z' && (!!shift ^ caps))
    c -= 32;
  if (shift && code != 55 && code != 74 && code != 78) {
    static const char from[] = "1234567890-=[];'`,./\\",
                      to[] = "!@#$%^&*()_+{}:\"~<>?|";
    for (unsigned i = 0; from[i]; i++)
      if (c == from[i]) {
        c = to[i];
        break;
      }
  }
  return (unsigned char)c;
}
