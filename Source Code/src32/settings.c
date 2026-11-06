#include "locale.h"
/* Persistent display and language preferences, with timed display rollback. */
#include "settings.h"
#include "graphics.h"
#include "vfs.h"
#include "storage.h"
#include "disk.h"
#include "lib/kstring.h"
#include "mm/page.h"
#define INK 0x253044u
#define BLUE 0x3265a8u
int settings_language, settings_pending;
static int tab = 1, rw = 1024, rh = 768, rs = 100, rc = 100, rl;
static int ow, oh, os, oc, ol;
static int slider_drag;
static u32 deadline, last_count;
static TextKey settings_status_key =
    SETTINGS_STATUS_CHOOSE_YOUR_SETTINGS_THEN_SELECT_APPLY;
static void settings_set_status(TextKey key) {
  settings_status_key = key;
}
static const int modes[][2] = {{800, 600},  {1024, 768}, {1280, 720},
                               {1280, 960}, {1600, 900}, {1920, 1080}};
static const char* mode_names[] = {"800 x 600",  "1024 x 768", "1280 x 720",
                                   "1280 x 960", "1600 x 900", "1920 x 1080"};
static const int scales[] = {100, 125, 150, 200};
typedef struct {
  u32 magic, version, width, height, scale, contrast, language, reserved;
} Preferences;
#define PREF_MAGIC 0x32474643u
static int hit(int x, int y, int a, int b, int w, int h) {
  return x >= a && x < a + w && y >= b && y < b + h;
}
static void choice(int x, int y, int w, const char* text, int selected) {
  g_round(x, y, w, 30, 4, selected ? 0xdbe9fb : 0xf0f2f6);
  g_box(x, y, w, 30, selected ? BLUE : 0xc7ced8,
        selected ? 0xdbe9fb : 0xf0f2f6);
  g_center(x, y + 5, w, text, selected ? BLUE : INK);
}
void settings_language_set(int language) {
  settings_language = language ? 1 : 0;
  if (gui_active)
    gui_refresh();
}
static int valid(const Preferences* p) {
  return p->magic == PREF_MAGIC && p->version == 1 && p->width >= 800 &&
         p->width <= 1920 && p->height >= 540 && p->height <= 1200 &&
         p->scale >= 100 && p->scale <= 200 && p->contrast >= 60 &&
         p->contrast <= 140 && p->language <= 1;
}
void settings_init(void) {
  Preferences p;
  int n = vfs_lookup("/home/user/.sino-settings");
  int found =
      n >= 0 && vfs_read(n, (u8*)&p, sizeof(p)) == (int)sizeof(p) && valid(&p);
  if (!found && *(volatile u8*)0x4f0c >= 0x80) {
    u8 sector[512];
    int d = storage_disk >= 0 ? storage_disk : *(volatile u8*)0x4f0c - 0x80;
    if (d < 4 && disk_read(d, 1, sector)) {
      memcpy(&p, sector, sizeof(p));
      found = valid(&p);
    }
  }
  if (found) {
    rw = (int)p.width;
    rh = (int)p.height;
    rs = (int)p.scale;
    rc = (int)p.contrast;
    settings_language = rl = (int)p.language;
  }
}
void settings_boot(void) {
  if (!graphics_mode(rw, rh, rs, rc)) {
    rw = 1024;
    rh = 768;
    rs = rc = 100;
    (void)graphics_mode(rw, rh, rs, rc);
  }
}
static void stage_current(void) {
  rw = display_width;
  rh = display_height;
  rs = display_scale;
  rc = display_contrast;
  rl = settings_language;
}
void settings_open(void) {
  if (!settings_pending) {
    stage_current();
    settings_set_status(SETTINGS_STATUS_CHOOSE_YOUR_SETTINGS_THEN_SELECT_APPLY);
  }
}
static void persist(void) {
  Preferences p = {PREF_MAGIC,
                   1,
                   (u32)display_width,
                   (u32)display_height,
                   (u32)display_scale,
                   (u32)display_contrast,
                   (u32)settings_language,
                   0};
  int n = vfs_lookup("/home/user/.sino-settings");
  if (n < 0)
    n = vfs_create("/home/user/.sino-settings", 0, 0);
  if (n < 0 || vfs_write(n, (const u8*)&p, sizeof(p), 0) < 0) {
    settings_set_status(
        SETTINGS_ERROR_SETTINGS_COULD_NOT_BE_SAVED_CHECK_THE_DATA_DISK);
    return;
  }
  if (!storage_ready) {
    settings_set_status(
        SETTINGS_STATUS_APPLIED_FOR_THIS_SESSION_NO_PERSISTENT_DISK);
    return;
  }
  settings_set_status(
      storage_sync()
          ? SETTINGS_STATUS_SETTINGS_SAVED_AND_WILL_SURVIVE_A_RESTART
          : SETTINGS_ERROR_SETTINGS_COULD_NOT_BE_SAVED_CHECK_THE_DATA_DISK);
}
void settings_cancel(void) {
  if (!settings_pending)
    return;
  (void)graphics_mode(ow, oh, os, oc);
  settings_language = ol;
  settings_pending = 0;
  stage_current();
  settings_set_status(SETTINGS_STATUS_PREVIOUS_DISPLAY_SETTINGS_RESTORED);
  gui_refresh();
}
static void apply(void) {
  if (settings_pending)
    return;
  ow = display_width;
  oh = display_height;
  os = display_scale;
  oc = display_contrast;
  ol = settings_language;
  if (!graphics_mode(rw, rh, rs, rc)) {
    settings_set_status(
        SETTINGS_ERROR_THIS_RESOLUTION_AND_SCALE_COMBINATION_IS_UNAVAILABLE);
    return;
  }
  settings_language = rl;
  settings_pending = 1;
  deadline = ticks + 1500;
  last_count = 16;
  settings_set_status(TEXT_NONE);
  gui_refresh();
}
int settings_poll(void) {
  if (!settings_pending)
    return 0;
  if ((i32)(ticks - deadline) >= 0) {
    settings_cancel();
    return 1;
  }
  u32 count = (deadline - ticks + 99) / 100;
  if (count != last_count) {
    last_count = count;
    return 1;
  }
  return 0;
}
int settings_mouse(int x, int y, int held) {
  (void)y;
  if (!held) {
    slider_drag = 0;
    return 0;
  }
  if (!slider_drag || settings_pending)
    return 0;
  int next = 60 + (x - 185) * 80 / 350;
  if (next < 60)
    next = 60;
  if (next > 140)
    next = 140;
  if (next == rc)
    return 0;
  rc = next;
  return 1;
}
void settings_draw(int w, int h) {
  g_fill(2, 30, w - 4, h - 32, 0xf9fafc);
  g_fill(2, 30, 155, h - 32, 0xebeff5);
  g_text(1, 18, 48, locale_text(SETTINGS_LABEL_SETTINGS), INK);
  const char* tabs[] = {
      locale_text(SETTINGS_LABEL_SYSTEM), locale_text(SETTINGS_LABEL_DISPLAY),
      locale_text(SETTINGS_LABEL_LANGUAGE), locale_text(SETTINGS_LABEL_ABOUT)};
  for (int i = 0; i < 4; i++) {
    if (tab == i)
      g_round(10, 99 + i * 44, 137, 36, 4, 0xd4e3f6);
    g_text(0, 27, 108 + i * 44, tabs[i], tab == i ? BLUE : INK);
  }
  int x = 181;
  g_text(1, x, 48, tabs[tab], INK);
  if (tab == 0) {
    g_text(0, x, 100, locale_text(SETTINGS_LABEL_SINO_OPERATING_SYSTEM), INK);
    g_text(0, x, 140, locale_text(SETTINGS_LABEL_VERSION_0_10_SINO32), BLUE);
    g_text(0, x, 178,
           locale_text(
               SETTINGS_LABEL_32_BIT_X86_CUSTOM_KERNEL_DRIVERS_AND_DESKTOP),
           INK);
    g_text(0, x, 216,
           *(volatile u8*)0x4f0c >= 0x80
               ? locale_text(SETTINGS_LABEL_BOOT_SOURCE_INSTALLED_HARD_DISK)
               : locale_text(SETTINGS_LABEL_BOOT_SOURCE_ISO_LIVE_SESSION),
           INK);
    g_text(0, x, 254,
           storage_ready
               ? locale_text(SETTINGS_LABEL_HOME_FILES_PERSISTENT_DATA_VOLUME)
               : locale_text(SETTINGS_LABEL_HOME_FILES_TEMPORARY_MEMORY_ONLY),
           INK);
    char b[16];
    number(free_pages * 4, b);
    g_text(0, x, 292, locale_text(SETTINGS_LABEL_FREE_MEMORY_KB), INK);
    g_text(0, x + 160, 292, b, BLUE);
    choice(x, 345, 165, locale_text(SETTINGS_LABEL_DISPLAY_SETTINGS), 0);
    choice(x + 180, 345, 165, locale_text(SETTINGS_LABEL_LANGUAGE_SETTINGS), 0);
  } else if (tab == 1) {
    g_text(0, x, 96, locale_text(SETTINGS_LABEL_SCREEN_RESOLUTION), INK);
    for (int i = 0; i < 6; i++)
      choice(x + (i % 3) * 166, 126 + (i / 3) * 38, 155, mode_names[i],
             rw == modes[i][0] && rh == modes[i][1]);
    g_text(0, x, 212, locale_text(SETTINGS_LABEL_SCALE), INK);
    for (int i = 0; i < 4; i++) {
      char b[16];
      number((u32)scales[i], b);
      u32 n = strlen(b);
      b[n] = '%';
      b[n + 1] = 0;
      choice(x + i * 110, 242, 99, b, rs == scales[i]);
    }
    g_text(0, x, 296, locale_text(SETTINGS_LABEL_CONTRAST), INK);
    g_fill(x + 4, 343, 350, 4, 0xc4ccd7);
    int pos = x + 4 + (rc - 60) * 350 / 80;
    g_round(pos - 6, 335, 13, 21, 5, BLUE);
    char b[16];
    number((u32)rc, b);
    g_text(0, x + 381, 335, b, INK);
    g_text(0, x, 381,
           locale_text(
               SETTINGS_LABEL_SCALE_CHANGES_TEXT_CONTROLS_AND_DESKTOP_SPACE),
           0x667388);
  } else if (tab == 2) {
    g_text(0, x, 100, locale_text(SETTINGS_LABEL_DISPLAY_LANGUAGE), INK);
    choice(x, 145, 212, locale_text(SETTINGS_LABEL_CHINESE_SIMPLIFIED),
           rl == 0);
    choice(x + 228, 145, 212, "English", rl == 1);
    g_text(
        0, x, 211,
        locale_text(
            SETTINGS_LABEL_YOUR_FILENAMES_AND_DOCUMENT_CONTENTS_REMAIN_UNCHANGED),
        0x667388);
    g_text(0, x, 257,
           locale_text(SETTINGS_LABEL_CHINESE_INPUT_CTRL_SPACE_TOGGLES_PINYIN),
           INK);
    g_text(0, x, 293, locale_text(SETTINGS_LABEL_SPACE_SELECTS_1_7_CHOOSE_PAGE),
           INK);
    g_text(
        0, x, 329,
        locale_text(
            SETTINGS_LABEL_CHARACTERS_AND_COMMON_PHRASES_NO_SENTENCE_PREDICTION),
        0x667388);
  } else {
    g_text(1, x, 104, "SINO 0.10", BLUE);
    g_text(
        0, x, 153,
        locale_text(SETTINGS_LABEL_KERNEL_DESKTOP_INSTALLER_CUSTOM_C_ASSEMBLY),
        INK);
    g_text(
        0, x, 195,
        locale_text(SETTINGS_LABEL_OPEN_SOURCE_QEMU_UNIFONT_AND_CLASSIC_FONTS),
        INK);
    g_text(0, x, 237,
           locale_text(SETTINGS_LABEL_PINYIN_CHARACTER_DATA_PINYIN_DATA_MIT),
           INK);
    g_text(
        0, x, 279,
        locale_text(SETTINGS_LABEL_BIOS_MBR_BOOT_NATIVE_SINO_DATA_FILESYSTEM),
        INK);
    g_text(
        0, x, 321,
        locale_text(
            SETTINGS_LABEL_NATIVE_APPS_USE_USER_MODE_LEGACY_APPS_REMAIN_IN_KERNEL),
        0x667388);
    g_text(
        0, x, 359,
        locale_text(SETTINGS_LABEL_SOURCE_AND_COMPONENT_LICENSES_ARE_INCLUDED),
        0x667388);
  }
  if (settings_pending) {
    int y = h - 149;
    g_box(174, y, w - 190, 136, BLUE, 0xe8f0fc);
    g_text(
        0, 190, y + 12,
        locale_text(
            SETTINGS_LABEL_KEEP_THESE_DISPLAY_SETTINGS_THEY_REVERT_AUTOMATICALLY),
        INK);
    char b[12];
    number((deadline - ticks + 99) / 100, b);
    g_text(0, 190, y + 40, b, BLUE);
    g_text(0, 227, y + 40, locale_text(SETTINGS_LABEL_SECONDS), BLUE);
    choice(w - 266, y + 85, 112, locale_text(SETTINGS_LABEL_REVERT), 0);
    choice(w - 142, y + 85, 112, locale_text(SETTINGS_LABEL_KEEP_CHANGES), 1);
  } else {
    g_cliptext(x, h - 88, w - x - 18, locale_text(settings_status_key),
               0x667388);
    choice(w - 266, h - 49, 112, locale_text(SETTINGS_LABEL_DEFAULTS), 0);
    choice(w - 142, h - 49, 112, locale_text(SETTINGS_LABEL_APPLY), 1);
  }
}
int settings_click(int x, int y, int w, int h) {
  if (settings_pending) {
    if (hit(x, y, w - 266, h - 64, 112, 30))
      settings_cancel();
    else if (hit(x, y, w - 142, h - 64, 112, 30)) {
      persist();
      settings_pending = 0;
      gui_refresh();
    }
    return 1;
  }
  for (int i = 0; i < 4; i++)
    if (hit(x, y, 10, 99 + i * 44, 137, 36)) {
      tab = i;
      return 1;
    }
  if (hit(x, y, w - 266, h - 49, 112, 30)) {
    rw = 1024;
    rh = 768;
    rs = rc = 100;
    rl = 0;
    settings_set_status(
        SETTINGS_STATUS_SELECT_APPLY_TO_RESTORE_THE_DEFAULT_SETTINGS);
    return 1;
  }
  if (hit(x, y, w - 142, h - 49, 112, 30)) {
    apply();
    return 1;
  }
  if (tab == 0 && y >= 345 && y < 375) {
    if (x >= 181 && x < 346)
      tab = 1;
    else if (x >= 361 && x < 526)
      tab = 2;
  }
  if (tab == 1) {
    for (int i = 0; i < 6; i++)
      if (hit(x, y, 181 + (i % 3) * 166, 126 + (i / 3) * 38, 155, 30)) {
        rw = modes[i][0];
        rh = modes[i][1];
        if (rw * 100 / rs > 1280 || rh * 100 / rs > 960)
          rs = 150;
      }
    for (int i = 0; i < 4; i++)
      if (hit(x, y, 181 + i * 110, 242, 99, 30))
        rs = scales[i];
    if (hit(x, y, 179, 327, 370, 35)) {
      slider_drag = 1;
      rc = 60 + (x - 185) * 80 / 350;
      if (rc < 60)
        rc = 60;
      if (rc > 140)
        rc = 140;
    }
  } else if (tab == 2) {
    if (hit(x, y, 181, 145, 212, 30))
      rl = 0;
    else if (hit(x, y, 409, 145, 212, 30))
      rl = 1;
  }
  return 1;
}
