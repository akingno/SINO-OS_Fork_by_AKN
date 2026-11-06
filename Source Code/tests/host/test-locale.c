#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SINO_KERNEL_H
#include "../../src32/locale.h"
#include "../../src32/vfs.h"
int settings_language;
static void check(const char* id, int ok) {
  printf("CHECK\t%s\t%d\n", id, !!ok);
}
int main(void) {
  int complete = 1;
  for (int key = 1; key < TEXT_KEY_COUNT; key++)
    for (int language = 0; language < 2; language++)
      if (!locale_text_for(language, (TextKey)key)[0])
        complete = 0;
  check("locale.complete-catalog", complete);
  check("locale.language-fallback",
        !strcmp(locale_text_for(99, GUI_LABEL_DESKTOP), "Desktop"));
  check("locale.invalid-key",
        !strcmp(locale_text((TextKey)-1), "[invalid text key]") &&
            !strcmp(locale_text(TEXT_KEY_COUNT), "[invalid text key]"));
  char out[128], expected[128];
  settings_language = 0;
  sprintf(expected, "文件表已满（%d 个项目）。", VFS_NODES);
  check("locale.vfs-node-limit",
        locale_format(out, sizeof(out), VFS_ERROR_NODE_LIMIT, "", VFS_NODES) >
                0 &&
            !strcmp(out, expected));
  settings_language = 1;
  sprintf(expected, "The file table is full (%d entries).", VFS_NODES);
  check("locale.vfs-node-limit-en",
        locale_format(out, sizeof(out), VFS_ERROR_NODE_LIMIT, "", VFS_NODES) >
                0 &&
            !strcmp(out, expected));
  check("locale.user-text-preserved",
        locale_format(out, sizeof(out), GUI_TEMPLATE_FILE_PROPERTIES, "桌面",
                      42) > 0 &&
            !strcmp(out, "桌面  File  42 bytes"));
  check("locale.folder-properties",
        locale_format(out, sizeof(out), GUI_TEMPLATE_FOLDER_PROPERTIES, "桌面",
                      0) > 0 &&
            !strcmp(out, "桌面  Folder  0 bytes"));
  check("locale.format-overflow",
        locale_format(out, 4, GUI_TEMPLATE_FILE_PROPERTIES, "桌面", 42) == -1 &&
            !out[0]);
  check("locale.format-zero-capacity",
        locale_format(out, 0, GUI_TEMPLATE_FILE_PROPERTIES, "", 0) == -1);
  settings_language = 0;
  const char* zh = locale_text(GUI_LABEL_DESKTOP);
  settings_language = 1;
  check("locale.live-language",
        !strcmp(zh, "桌面") &&
            !strcmp(locale_text(GUI_LABEL_DESKTOP), "Desktop"));
  return 0;
}
