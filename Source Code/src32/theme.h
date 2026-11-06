/* UI color theme. Define THEME_VIRIDIAN (here or via -DTHEME_VIRIDIAN) to
   switch from the original SINO Platinum palette to the Viridian deep-teal
   theme. Both gui.c and user/terminal_frontend.c include this header. */
#ifndef THEME_H
#define THEME_H

#define THEME_VIRIDIAN

#ifdef THEME_VIRIDIAN
/* Viridian: deep-teal wallpaper + cool platinum windows + cerulean folders */

#define BG   0xd6e0e0u
#define INK  0x182224u
#define BLUE 0x1b6ca8u

#define DESKTOP_BASE      0x015452u
#define DESKTOP_DOT       0x09605eu
#define DESKTOP_WATERMARK 0x1d6b69u
#define DESKTOP_VERSION   0x2b7a78u

#define FOLDER_BODY    0x52a8dcu
#define FOLDER_BORDER  0x1c4561u
#define FOLDER_DIVIDER 0xdbf0fau

#define WINDOW_BORDER   0x1d2b2bu
#define WINDOW_BODY     0xf4f8f8u
#define TITLEBAR_FOCUS  0xd6e0e0u
#define TITLEBAR_BLUR   0xe5ededu
#define TITLE_TEXT_BLUR 0x788a8au
#define TITLEBAR_LINE   0x7a8c8cu
#define WINDOW_SHADOW   0x002b2au

#define MENUBAR_BG   0xe8f0f0u
#define MENUBAR_LINE 0x1d2b2bu

#define TERM_BG   0x061216u
#define TERM_TEXT 0xdae8e8u
#define TERM_HINT 0x48a8b0u

#else
/* Original SINO Platinum palette */

#define BG   0xd6d7dbu
#define INK  0x24252au
#define BLUE 0x355e9au

#define DESKTOP_BASE      0x718b9au
#define DESKTOP_DOT       0x7892a0u
#define DESKTOP_WATERMARK 0xb4c8d0u
#define DESKTOP_VERSION   0xd0dce0u

#define FOLDER_BODY    0x95b4cfu
#define FOLDER_BORDER  0x4c6377u
#define FOLDER_DIVIDER 0xdbe7f0u

#define WINDOW_BORDER   0x383b42u
#define WINDOW_BODY     0xfafbfcu
#define TITLEBAR_FOCUS  0xd3d4d9u
#define TITLEBAR_BLUR   0xe2e3e7u
#define TITLE_TEXT_BLUR 0x878c96u
#define TITLEBAR_LINE   0x818690u
#define WINDOW_SHADOW   0x4b5c69u

#define MENUBAR_BG   0xe8e9edu
#define MENUBAR_LINE 0x343a44u

#define TERM_BG   0x171c25u
#define TERM_TEXT 0xd6dce6u
#define TERM_HINT 0x94ade3u

#endif
#endif
