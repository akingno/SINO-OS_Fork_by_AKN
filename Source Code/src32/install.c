#include "locale.h"
#include "klog.h"
/* Graphical BIOS/MBR installer. Partition edits remain staged until
 * confirmation. */
#include "install.h"
#include "graphics.h"
#include "disk.h"
#include "storage.h"
#include "settings.h"
#define BLUE 0x245b9eu
#define INK 0x26354bu
typedef struct {
  u32 start, size;
  int type, format;
} Partition;
typedef struct {
  u32 start, size;
  int slot;
} Region;
static Partition parts[4];
static Region regions[9];
static int region_count, selected, selected_disk = -1, size_edit, size_len,
                                   table_error;
static char size_text[12];
static TextKey install_message_key = TEXT_NONE;
static void install_set_message(TextKey key) {
  install_message_key = key;
}
static int system_slot = -1, data_slot = -1, copy_phase;
static u32 copied, verified;
static u8 block[512], check[512], mbr[512];
int install_step, install_complete;
static int hit(int x, int y, int a, int b, int w, int h) {
  return x >= a && x < a + w && y >= b && y < b + h;
}
static void button(int x, int y, int w, const char* text, int primary) {
  g_round(x, y, w, 31, 4, primary ? BLUE : 0xe8edf5);
  g_center(x, y + 6, w, text, primary ? 0xffffff : INK);
}
static void size_label(u32 sectors, char* out) {
  number(sectors / 2048, out);
  u32 n = strlen(out);
  const char* s = " MB";
  while ((*((out++) + n) = *s++)) {
  }
}
static void rows(void) {
  int slots[4], n = 0;
  for (int i = 0; i < 4; i++)
    if (parts[i].type)
      slots[n++] = i;
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (parts[slots[j]].start < parts[slots[i]].start) {
        int t = slots[i];
        slots[i] = slots[j];
        slots[j] = t;
      }
  region_count = 0;
  u32 cursor = 2048;
  for (int i = 0; i < n; i++) {
    Partition* p = &parts[slots[i]];
    if (p->start > cursor)
      regions[region_count++] = (Region){cursor, p->start - cursor, -1};
    regions[region_count++] = (Region){p->start, p->size, slots[i]};
    cursor = p->start + p->size;
  }
  if (selected_disk >= 0 && cursor < disks[selected_disk].sectors &&
      region_count < 9)
    regions[region_count++] =
        (Region){cursor, disks[selected_disk].sectors - cursor, -1};
  if (selected >= region_count)
    selected = region_count - 1;
}
static void load_disk(int d) {
  selected_disk = d;
  selected = 0;
  table_error = 0;
  memset(parts, 0, sizeof(parts));
  install_set_message(TEXT_NONE);
  if (d < 0 || !disk_read(d, 0, block)) {
    install_set_message(INSTALL_ERROR_CANNOT_READ_THE_DISK);
    table_error = 1;
    return;
  }
  if (block[510] == 0x55 && block[511] == 0xaa) {
    for (int i = 0; i < 4; i++) {
      const u8* p = block + 446 + i * 16;
      u32 start = *(const u32*)(p + 8), size = *(const u32*)(p + 12);
      if (!p[4])
        continue;
      if (p[4] == 0xee) {
        table_error = 1;
        install_set_message(INSTALL_ERROR_GPT_UNSUPPORTED);
        break;
      }
      if (start < 2048 || !size || start >= disks[d].sectors ||
          size > disks[d].sectors - start) {
        table_error = 1;
        install_set_message(
            INSTALL_ERROR_INVALID_PARTITION_TABLE_DISK_WRITES_ARE_DISABLED);
        break;
      }
      parts[i] = (Partition){start, size, p[4], 0};
    }
    for (int i = 0; i < 4; i++)
      for (int j = i + 1; j < 4; j++)
        if (parts[i].type && parts[j].type &&
            parts[i].start < parts[j].start + parts[j].size &&
            parts[j].start < parts[i].start + parts[i].size) {
          table_error = 1;
          install_set_message(
              INSTALL_ERROR_OVERLAPPING_PARTITIONS_DISK_WRITES_ARE_DISABLED);
        }
  } else {
    int blank = 1;
    for (int i = 0; i < 512; i++)
      if (block[i])
        blank = 0;
    if (!blank) {
      table_error = 1;
      install_set_message(
          INSTALL_ERROR_UNKNOWN_DISK_LAYOUT_AUTOMATIC_WRITES_ARE_DISABLED);
    }
  }
  rows();
}
void install_open(void) {
  install_step = install_complete = size_edit = 0;
  install_set_message(TEXT_NONE);
  int d = -1;
  for (int i = 0; i < 4; i++)
    if (disks[i].present && disks[i].sectors >= 131072) {
      d = i;
      break;
    }
  load_disk(d);
}
int install_offer(void) {
  if (*(volatile u8*)0x4f0c >= 0x80)
    return 0;
  for (int d = 0; d < 4; d++)
    if (disks[d].present && disks[d].sectors >= 131072)
      return 1;
  return 0;
}
int install_can_close(void) {
  return install_step != 5;
}
static void automatic(void) {
  if (selected_disk < 0 || table_error)
    return;
  for (int i = 0; i < 4; i++)
    if (parts[i].type) {
      install_set_message(
          INSTALL_STATUS_DELETE_EXISTING_PARTITIONS_BEFORE_AUTOMATIC_PARTITIONING);
      return;
    }
  if (disks[selected_disk].sectors < 131072) {
    install_set_message(
        INSTALL_ERROR_SETUP_NEEDS_A_DEDICATED_DISK_OF_AT_LEAST_64_MB);
    return;
  }
  parts[0] = (Partition){2048, 32768, 1, 1};
  parts[1] = (Partition){34816, disks[selected_disk].sectors - 34816, 0x7f, 1};
  selected = 0;
  rows();
  install_set_message(
      INSTALL_STATUS_SYSTEM_AND_DATA_PARTITIONS_PLANNED_NO_DISK_WRITES_YET);
}
static void new_partition(void) {
  if (selected_disk < 0 || table_error || selected < 0 ||
      selected >= region_count)
    return;
  Region* r = &regions[selected];
  if (r->slot >= 0) {
    install_set_message(INSTALL_ERROR_SELECT_UNALLOCATED_SPACE);
    return;
  }
  int have_system = 0;
  for (int i = 0; i < 4; i++)
    if (parts[i].type == 1)
      have_system = 1;
  u32 mb = have_system ? r->size / 2048 : 16;
  if (mb > r->size / 2048)
    mb = r->size / 2048;
  number(mb, size_text);
  size_len = (int)strlen(size_text);
  size_edit = 1;
}
static void create_partition(void) {
  u32 mb = 0;
  for (int i = 0; i < size_len; i++) {
    if (mb > 131072) {
      install_set_message(INSTALL_ERROR_PARTITION_SIZE_IS_OUT_OF_RANGE);
      return;
    }
    mb = mb * 10 + (u32)(size_text[i] - '0');
  }
  if (!mb) {
    install_set_message(INSTALL_ERROR_ENTER_A_PARTITION_SIZE_GREATER_THAN_ZERO);
    return;
  }
  Region r = regions[selected];
  if (mb > r.size / 2048) {
    install_set_message(
        INSTALL_ERROR_THAT_SIZE_EXCEEDS_THE_SELECTED_UNALLOCATED_SPACE);
    return;
  }
  int slot = -1, type = 1;
  for (int i = 0; i < 4; i++) {
    if (!parts[i].type && slot < 0)
      slot = i;
    if (parts[i].type == 1)
      type = 0x7f;
  }
  if (slot < 0) {
    install_set_message(
        INSTALL_ERROR_MBR_SUPPORTS_AT_MOST_FOUR_PRIMARY_PARTITIONS);
    return;
  }
  if ((type == 1 && mb < 4) || (type == 0x7f && mb < 16)) {
    install_set_message(
        INSTALL_ERROR_SYSTEM_NEEDS_4_MB_DATA_NEEDS_AT_LEAST_16_MB);
    return;
  }
  parts[slot] = (Partition){r.start, mb * 2048, type, 1};
  size_edit = 0;
  rows();
  install_set_message(
      INSTALL_STATUS_PARTITION_ADDED_TO_THE_PLAN_NO_DISK_WRITES_YET);
}
static int plan(void) {
  system_slot = data_slot = -1;
  if (selected_disk < 0 || table_error)
    return 0;
  if (disks[selected_disk].sectors < 131072) {
    install_set_message(
        INSTALL_ERROR_SETUP_NEEDS_A_DEDICATED_DISK_OF_AT_LEAST_64_MB);
    return 0;
  }
  for (int i = 0; i < 4; i++) {
    if (parts[i].type == 1 && parts[i].size >= BOOT_SECTORS && system_slot < 0)
      system_slot = i;
    if (parts[i].type == 0x7f && parts[i].size >= 16392 && data_slot < 0)
      data_slot = i;
  }
  if (system_slot < 0 || data_slot < 0) {
    install_set_message(
        INSTALL_ERROR_A_SYSTEM_PARTITION_AND_A_SINO_DATA_PARTITION_ARE_REQUIRED);
    return 0;
  }
  if (*(volatile u8*)0x4f0c >= 0x80 && selected_disk == storage_disk) {
    install_set_message(
        INSTALL_ERROR_BOOT_THE_ISO_TO_REINSTALL_THIS_SYSTEM_IS_CURRENTLY_RUNNING);
    return 0;
  }
  return 1;
}
static void begin(void) {
  if (!plan())
    return;
  if (storage_ready && storage_disk == selected_disk) {
    if (!storage_sync()) {
      install_set_message(
          INSTALL_ERROR_THE_CURRENT_DATA_VOLUME_CANNOT_SYNC_SETUP_STOPPED);
      return;
    }
    storage_ready = storage_dirty = 0;
  }
  int file = -1;
  for (u32 i = 0; i < file_count; i++)
    if (streq(files[i].name, "MBR.BIN"))
      file = (int)i;
  if (file < 0 || !fs_read((u32)file, mbr, 512)) {
    install_set_message(INSTALL_ERROR_MBR_BOOT_FILE_MISSING);
    return;
  }
  memset(mbr + 446, 0, 64);
  for (int i = 0; i < 4; i++)
    if (parts[i].type) {
      u8* p = mbr + 446 + i * 16;
      p[0] = i == system_slot ? 0x80 : 0;
      p[1] = p[5] = 0xfe;
      p[2] = p[3] = p[6] = p[7] = 0xff;
      p[4] = (u8)parts[i].type;
      *(u32*)(p + 8) = parts[i].start;
      *(u32*)(p + 12) = parts[i].size;
    }
  *(u32*)(mbr + 440) = 0x53494e4f ^ ticks;
  copied = verified = 0;
  copy_phase = 0;
  install_step = 5;
  install_set_message(TEXT_NONE);
}
int install_poll(void) {
  if (install_step != 5)
    return 0;
  int ok = 1;
  u32 base = parts[system_slot].start;
  if (copy_phase == 0) {
    for (int n = 0; n < 32 && copied < BOOT_SECTORS; n++, copied++) {
      memcpy(block, RAMDISK + copied * 512, 512);
      if (!copied) {
        *(u32*)(block + 28) = base;
        block[36] = (u8)(0x80 + selected_disk);
      }
      if (!disk_write(selected_disk, base + copied, block)) {
        ok = 0;
        break;
      }
    }
    if (copied == BOOT_SECTORS) {
      ok = disk_flush(selected_disk);
      copy_phase = 1;
    }
  } else if (copy_phase == 1) {
    for (int n = 0; n < 32 && verified < BOOT_SECTORS; n++, verified++) {
      if (!disk_read(selected_disk, base + verified, check)) {
        ok = 0;
        break;
      }
      memcpy(block, RAMDISK + verified * 512, 512);
      if (!verified) {
        *(u32*)(block + 28) = base;
        block[36] = (u8)(0x80 + selected_disk);
      }
      for (int j = 0; j < 512; j++)
        if (check[j] != block[j])
          ok = 0;
      if (!ok)
        break;
    }
    if (verified == BOOT_SECTORS)
      copy_phase = 2;
  } else {
    u32 data = parts[data_slot].start;
    memset(block, 0, 512);
    if (parts[data_slot].format) {
      for (int i = 0; i < 9; i++)
        if (!disk_write(selected_disk, data + (u32)i, block))
          ok = 0;
      if (!disk_write(selected_disk, data + 8200, block))
        ok = 0;
    }
    if (ok) {
      u32* p = (u32*)block;
      p[0] = 0x32474643;
      p[1] = 1;
      p[2] = (u32)display_width;
      p[3] = (u32)display_height;
      p[4] = (u32)display_scale;
      p[5] = (u32)display_contrast;
      p[6] = (u32)settings_language;
      ok = disk_write(selected_disk, 1, block) && disk_flush(selected_disk);
    }
    if (ok)
      ok = disk_write(selected_disk, 0, mbr) && disk_flush(selected_disk);
    if (ok) {
      install_complete = 1;
      install_step = 6;
      pr_debug(
          "INSTALL completed: MBR, boot partition verified, SFS data partition "
          "ready\n");
    }
  }
  if (!ok) {
    pr_debug("INSTALL I/O failure: phase=");
    pr_debug_hex((u32)copy_phase);
    pr_debug(" copied=");
    pr_debug_hex(copied);
    pr_debug(" verified=");
    pr_debug_hex(verified);
    pr_debug("\n");
    install_step = 3;
    install_complete = 0;
    install_set_message(
        INSTALL_ERROR_WRITE_OR_VERIFICATION_FAILED_INSTALLATION_IS_INCOMPLETE);
  }
  return 1;
}
static void restart(void) {
  cli();
  outb(0x64, 0xfe);
  for (;;)
    halt();
}
void install_draw(int w, int h) {
  g_fill(2, 30, w - 4, h - 32, 0xf9fbfe);
  g_fill(2, 30, 158, h - 32, BLUE);
  g_text(1, 21, 54, "SINO", 0xffffff);
  g_text(0, 21, 91, locale_text(INSTALL_LABEL_SETUP), 0xdceafd);
  const char* steps[] = {locale_text(INSTALL_LABEL_1_PREPARE),
                         locale_text(INSTALL_LABEL_2_CHOOSE_DISK),
                         locale_text(INSTALL_LABEL_3_INSTALL),
                         locale_text(INSTALL_LABEL_4_FINISH)};
  int active = install_step < 3    ? 0
               : install_step < 5  ? 1
               : install_step == 5 ? 2
                                   : 3;
  for (int i = 0; i < 4; i++)
    g_text(0, 21, 153 + i * 47, steps[i], i == active ? 0xffffff : 0xaec5e5);
  int x = 184;
  const char* titles[] = {
      locale_text(INSTALL_LABEL_LANGUAGE_AND_REGION),
      locale_text(INSTALL_LABEL_INSTALL_SINO),
      locale_text(INSTALL_LABEL_LICENSE_AND_INSTALLATION),
      locale_text(INSTALL_LABEL_WHERE_DO_YOU_WANT_TO_INSTALL_SINO),
      locale_text(INSTALL_LABEL_CONFIRM_INSTALLATION),
      locale_text(INSTALL_LABEL_INSTALLING_SINO),
      locale_text(INSTALL_LABEL_SINO_HAS_BEEN_INSTALLED)};
  g_text(1, x, 57, titles[install_step], INK);
  if (install_step == 0) {
    g_text(0, x, 125,
           locale_text(INSTALL_LABEL_CHOOSE_THE_SETUP_AND_SYSTEM_LANGUAGE),
           INK);
    button(x, 177, 210, locale_text(INSTALL_LABEL_CHINESE_SIMPLIFIED),
           settings_language == 0);
    button(x + 226, 177, 210, "English", settings_language == 1);
    g_text(0, x, 253,
           locale_text(
               INSTALL_LABEL_STANDARD_PC_KEYBOARD_WITH_BUILT_IN_BASIC_PINYIN),
           0x667388);
    g_text(0, x, 300,
           locale_text(
               INSTALL_LABEL_ONLY_THE_SELECTED_VIRTUAL_HARD_DISK_IS_TARGETED),
           0x667388);
  } else if (install_step == 1) {
    g_text(1, x, 139, "SINO 0.10", BLUE);
    g_text(0, x, 191,
           locale_text(
               INSTALL_LABEL_INSTALL_TO_A_HARD_DISK_AND_BOOT_WITHOUT_THE_CD),
           INK);
    g_text(
        0, x, 234,
        locale_text(
            INSTALL_LABEL_SETUP_PARTITIONS_COPIES_VERIFIES_AND_INSTALLS_BOOT_CODE),
        0x667388);
    g_text(
        0, x, 277,
        locale_text(INSTALL_LABEL_YOU_CAN_ALSO_EXIT_SETUP_AND_TRY_THE_DESKTOP),
        0x667388);
    button(x, 342, 217, locale_text(INSTALL_LABEL_INSTALL_NOW), 1);
    button(x + 232, 342, 170, locale_text(INSTALL_LABEL_TRY_DESKTOP), 0);
  } else if (install_step == 2) {
    g_text(0, x, 121,
           locale_text(
               INSTALL_LABEL_THE_SINO_KERNEL_DESKTOP_AND_INSTALLER_USE_MIT),
           INK);
    g_text(0, x, 163,
           locale_text(
               INSTALL_LABEL_QEMU_FONTS_AND_PINYIN_RETAIN_THEIR_OWN_LICENSES),
           INK);
    g_text(0, x, 205,
           locale_text(
               INSTALL_LABEL_CONTINUE_TO_USE_SINO_UNDER_THE_INCLUDED_LICENSES),
           INK);
    g_text(
        0, x, 268,
        locale_text(
            INSTALL_LABEL_SETUP_CHANGES_PARTITIONS_AND_DATA_ON_THE_SELECTED_DISK),
        0x9f473e);
    g_text(0, x, 310,
           locale_text(INSTALL_LABEL_BACK_UP_ANY_FILES_YOU_NEED_TO_KEEP_FIRST),
           0x9f473e);
    g_text(0, x, 366,
           locale_text(
               INSTALL_LABEL_BIOS_MBR_SUPPORTED_GPT_AND_UEFI_ARE_UNAVAILABLE),
           0x667388);
  } else if (install_step == 3) {
    for (int i = 0; i < 4; i++)
      if (disks[i].present) {
        char b[16] = "Disk ";
        number((u32)i, b + 5);
        button(x + i * 123, 107, 112, b, i == selected_disk);
      }
    if (selected_disk >= 0) {
      char b[24];
      size_label(disks[selected_disk].sectors, b);
      g_text(0, x, 147, disks[selected_disk].model, INK);
      g_text(0, w - 113, 147, b, INK);
    }
    g_fill(x, 177, w - x - 19, 27, 0xe8edf5);
    g_text(0, x + 12, 181,
           locale_text(INSTALL_LABEL_PARTITION_UNALLOCATED_SPACE), INK);
    g_text(0, w - 239, 181, locale_text(INSTALL_LABEL_CAPACITY), INK);
    g_text(0, w - 142, 181, locale_text(INSTALL_LABEL_TYPE), INK);
    int visible = (h - 380) / 34;
    if (visible < 1)
      visible = 1;
    int first = selected >= visible ? selected - visible + 1 : 0;
    for (int i = first; i < region_count && i < first + visible; i++) {
      int y = 207 + (i - first) * 34;
      Region* r = &regions[i];
      if (i == selected)
        g_fill(x, y, w - x - 19, 32, 0xddeafb);
      char b[24];
      if (r->slot < 0)
        g_text(0, x + 12, y + 7, locale_text(INSTALL_LABEL_UNALLOCATED_SPACE),
               INK);
      else {
        g_text(0, x + 12, y + 7,
               parts[r->slot].type == 1
                   ? locale_text(INSTALL_LABEL_SINO_SYSTEM_PARTITION)
               : parts[r->slot].type == 0x7f
                   ? locale_text(INSTALL_LABEL_SINO_DATA_PARTITION)
                   : locale_text(INSTALL_LABEL_OTHER_PARTITION),
               INK);
        if (parts[r->slot].format)
          g_text(0, x + 180, y + 7, locale_text(INSTALL_LABEL_WILL_FORMAT),
                 0x9f473e);
      }
      size_label(r->size, b);
      g_text(0, w - 239, y + 7, b, INK);
      g_text(0, w - 142, y + 7,
             r->slot < 0                ? locale_text(INSTALL_LABEL_FREE)
             : parts[r->slot].type == 1 ? "FAT12"
             : parts[r->slot].type == 0x7f
                 ? "SINO FS"
                 : locale_text(INSTALL_LABEL_PRESERVE),
             0x667388);
    }
    int y = h - 112;
    button(x, y, 112, locale_text(INSTALL_LABEL_NEW), 0);
    button(x + 123, y, 112, locale_text(INSTALL_LABEL_DELETE), 0);
    button(x + 246, y, 112, locale_text(INSTALL_LABEL_FORMAT), 0);
    button(x + 369, y, 126, locale_text(INSTALL_LABEL_AUTOMATIC), 0);
  } else if (install_step == 4) {
    char b[24];
    g_text(0, x, 123, locale_text(INSTALL_LABEL_TARGET_VIRTUAL_HARD_DISK), INK);
    if (selected_disk >= 0)
      g_text(0, x, 161, disks[selected_disk].model, BLUE);
    if (system_slot >= 0) {
      size_label(parts[system_slot].size, b);
      g_text(0, x, 211,
             locale_text(INSTALL_LABEL_SYSTEM_PARTITION_REPLACE_SYSTEM_FILES),
             INK);
      g_text(0, x + 340, 211, b, INK);
    }
    if (data_slot >= 0) {
      size_label(parts[data_slot].size, b);
      g_text(0, x, 253,
             parts[data_slot].format
                 ? locale_text(INSTALL_LABEL_DATA_PARTITION_FORMAT_AS_SINO_FS)
                 : locale_text(
                       INSTALL_LABEL_DATA_PARTITION_PRESERVE_EXISTING_CONTENT),
             INK);
      g_text(0, x + 340, 253, b, INK);
    }
    g_text(
        0, x, 320,
        locale_text(
            INSTALL_LABEL_EXISTING_CONTENT_ON_SELECTED_PARTITIONS_MAY_BE_LOST),
        0xa0443b);
    g_text(0, x, 363,
           locale_text(
               INSTALL_LABEL_THE_DISK_CHANGES_ONLY_AFTER_BEGIN_INSTALLATION),
           0x667388);
  } else if (install_step == 5) {
    u32 progress = (copied + verified) * 100 / 5760;
    g_text(
        0, x, 145,
        copy_phase == 0 ? locale_text(INSTALL_LABEL_COPYING_SYSTEM_FILES)
        : copy_phase == 1
            ? locale_text(INSTALL_LABEL_READING_BACK_AND_VERIFYING_SYSTEM_FILES)
            : locale_text(
                  INSTALL_LABEL_CONFIGURING_THE_DATA_PARTITION_AND_BOOT_CODE),
        INK);
    g_round(x, 218, w - x - 34, 16, 5, 0xe0e7f1);
    g_round(x, 218, (w - x - 34) * (int)progress / 100, 16, 5, BLUE);
    char b[12];
    number(progress, b);
    g_text(0, x, 261, b, BLUE);
    g_text(
        0, x, 330,
        locale_text(INSTALL_LABEL_KEEP_THE_VM_RUNNING_AND_THE_DISK_CONNECTED),
        0x667388);
  } else {
    g_text(0, x, 143,
           locale_text(
               INSTALL_LABEL_SYSTEM_FILES_COPIED_AND_VERIFIED_SECTOR_BY_SECTOR),
           INK);
    g_text(
        0, x, 191,
        locale_text(
            INSTALL_LABEL_MBR_BOOT_AND_A_PERSISTENT_DATA_PARTITION_ARE_READY),
        INK);
    g_text(0, x, 239,
           locale_text(INSTALL_LABEL_RESTART_TO_ENTER_SINO_FROM_THE_HARD_DISK),
           BLUE);
    g_text(
        0, x, 300,
        locale_text(
            INSTALL_LABEL_IF_THE_CD_BOOTS_AGAIN_EJECT_IT_AND_BOOT_THE_HARD_DISK),
        0x667388);
  }
  if (install_step != 5) {
    g_cliptext(x, h - 157, w - x - 24, locale_text(install_message_key),
               0xa0443b);
    if (install_step > 0 && install_step < 5)
      button(w - 282, h - 54, 110, locale_text(INSTALL_LABEL_BACK), 0);
    button(w - 158, h - 54, 132,
           install_step == 6   ? locale_text(INSTALL_LABEL_RESTART)
           : install_step == 4 ? locale_text(INSTALL_LABEL_BEGIN_INSTALLATION)
                               : locale_text(INSTALL_LABEL_NEXT),
           1);
  } else
    g_text(0, x, h - 58, locale_text(INSTALL_LABEL_PLEASE_WAIT), 0x667388);
  if (size_edit) {
    g_box(220, 174, 435, 196, BLUE, 0xf6f9fe);
    g_text(1, 241, 192, locale_text(INSTALL_LABEL_NEW_PARTITION), INK);
    g_text(0, 241, 237, locale_text(INSTALL_LABEL_SIZE_MB), INK);
    g_box(371, 233, 243, 33, 0x9eafc7, 0xffffff);
    g_text(0, 381, 239, size_text, INK);
    button(393, 310, 100, locale_text(INSTALL_LABEL_CANCEL), 0);
    button(507, 310, 110, locale_text(INSTALL_LABEL_CREATE), 1);
  }
}
int install_click(int x, int y, int w, int h) {
  if (size_edit) {
    if (hit(x, y, 393, 310, 100, 31))
      size_edit = 0;
    else if (hit(x, y, 507, 310, 110, 31))
      create_partition();
    return 1;
  }
  if (install_step == 5)
    return 1;
  if (install_step == 0) {
    if (hit(x, y, 184, 177, 210, 31))
      settings_language_set(0);
    else if (hit(x, y, 410, 177, 210, 31))
      settings_language_set(1);
  }
  if (install_step == 1) {
    if (hit(x, y, 184, 342, 217, 31))
      install_step = 2;
    else if (hit(x, y, 416, 342, 170, 31))
      return 2;
  }
  if (install_step == 3) {
    for (int i = 0; i < 4; i++)
      if (disks[i].present && hit(x, y, 184 + i * 123, 107, 112, 31))
        load_disk(i);
    int visible = (h - 380) / 34;
    if (visible < 1)
      visible = 1;
    int first = selected >= visible ? selected - visible + 1 : 0;
    if (x >= 184 && y >= 207 && y < 207 + visible * 34) {
      int row = first + (y - 207) / 34;
      if (row < region_count)
        selected = row;
    }
    int yy = h - 112;
    if (hit(x, y, 184, yy, 112, 31))
      new_partition();
    else if (hit(x, y, 307, yy, 112, 31) && !table_error && selected >= 0 &&
             selected < region_count) {
      int slot = regions[selected].slot;
      if (slot >= 0) {
        memset(&parts[slot], 0, sizeof(Partition));
        rows();
        install_set_message(
            INSTALL_STATUS_DELETION_STAGED_THE_DISK_IS_UNCHANGED);
      }
    } else if (hit(x, y, 430, yy, 112, 31) && !table_error && selected >= 0 &&
               selected < region_count) {
      int slot = regions[selected].slot;
      if (slot >= 0 && (parts[slot].type == 1 || parts[slot].type == 0x7f)) {
        parts[slot].format = 1;
        install_set_message(INSTALL_STATUS_FORMAT_STAGED_THE_DISK_IS_UNCHANGED);
      }
    } else if (hit(x, y, 553, yy, 126, 31))
      automatic();
  }
  if (install_step > 0 && install_step < 5 &&
      hit(x, y, w - 282, h - 54, 110, 31)) {
    install_step--;
    install_set_message(TEXT_NONE);
  } else if (hit(x, y, w - 158, h - 54, 132, 31)) {
    if (install_step == 3) {
      if (plan())
        install_step = 4;
    } else if (install_step == 4)
      begin();
    else if (install_step == 6)
      restart();
    else
      install_step++;
  }
  return 1;
}
int install_key(int key) {
  if (size_edit) {
    if (key == 27)
      size_edit = 0;
    else if (key == 13)
      create_partition();
    else if (key == 8 && size_len)
      size_text[--size_len] = 0;
    else if (key >= '0' && key <= '9' && size_len < 8) {
      size_text[size_len++] = (char)key;
      size_text[size_len] = 0;
    } else if (key == 1) {
      size_len = 0;
      size_text[0] = 0;
    }
    return 1;
  }
  if (install_step == 3 && (key == 0x148 || key == 0x150)) {
    selected += key == 0x148 ? -1 : 1;
    if (selected < 0)
      selected = 0;
    if (selected >= region_count)
      selected = region_count - 1;
    return 1;
  }
  return 0;
}
