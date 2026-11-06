#include "boot_fat12.h"
#include "../boot-ramdisk.h"
#include "../klog.h"
#include "../lib/kstring.h"

File files[224];
u32 file_count;
static u32 fat_offset, root_offset, data_offset, cluster_count, root_count;
static u32 cluster_bytes, ramdisk_bytes;
static u32 le16(const u8* p) {
  return p[0] | ((u32)p[1] << 8);
}
static u32 le32(const u8* p) {
  return le16(p) | (le16(p + 2) << 16);
}
int fs_init(void) {
  if (le16(RAMDISK + 510) != 0xaa55 || le16(RAMDISK + 11) != 512 ||
      (RAMDISK[13] != 1 && RAMDISK[13] != 2))
    return 0;
  ramdisk_bytes = le16(RAMDISK + 19) * 512;
  cluster_bytes = RAMDISK[13] * 512;
  if (!ramdisk_bytes || ramdisk_bytes > RAMDISK_SIZE) return 0;
  u32 reserved = le16(RAMDISK + 14), fats = RAMDISK[16],
      fat_size = le16(RAMDISK + 22);
  root_count = le16(RAMDISK + 17);
  fat_offset = reserved * 512;
  root_offset = (reserved + fats * fat_size) * 512;
  data_offset = root_offset + ((root_count * 32 + 511) / 512) * 512;
  if (!reserved || fats != 2 || !root_count || root_count > 224 || data_offset >= ramdisk_bytes || fat_size < 1)
    return 0;
  cluster_count = (ramdisk_bytes - data_offset) / cluster_bytes;
  if (cluster_count >= 4085 || (cluster_count + 2) * 3 / 2 + 1 > fat_size * 512)
    return 0;
  file_count = 0;
  for (u32 i = 0; i < root_count; i++) {
    const u8* e = RAMDISK + root_offset + i * 32;
    if (!e[0])
      break;
    if (e[0] == 0xe5 || (e[11] & 0x18))
      continue;
    File* f = &files[file_count++];
    u32 n = 0;
    for (u32 j = 0; j < 8; j++)
      if (e[j] != ' ')
        f->name[n++] = (char)e[j];
    if (e[8] != ' ') {
      f->name[n++] = '.';
      for (u32 j = 8; j < 11 && e[j] != ' '; j++)
        f->name[n++] = (char)e[j];
    }
    f->name[n] = 0;
    f->size = le32(e + 28);
    f->cluster = (u16)le16(e + 26);
  }
  pr_debug("FAT12 RAM disk ready, full boot image preloaded\n");
  return 1;
}
int fs_read(u32 index, u8* dest, u32 cap) {
  if (index >= file_count || files[index].size > cap)
    return 0;
  u32 left = files[index].size, c = files[index].cluster, steps = 0;
  while (left) {
    if (c < 2 || c >= cluster_count + 2 || ++steps > cluster_count)
      return 0;
    u32 off = data_offset + (c - 2) * cluster_bytes, n = left > cluster_bytes ? cluster_bytes : left;
    if (off + n > ramdisk_bytes)
      return 0;
    memcpy(dest, RAMDISK + off, n);
    dest += n;
    left -= n;
    u32 fat = fat_offset + c + c / 2;
    if (fat + 1 >= root_offset)
      return 0;
    u32 next = le16(RAMDISK + fat);
    c = (c & 1) ? next >> 4 : next & 0xfff;
  }
  return 1;
}

int fs_read_at(u32 index, u32 offset, u8* dest, u32 length) {
  if (index >= file_count || offset > files[index].size ||
      length > files[index].size - offset) return 0;
  u32 c = files[index].cluster, steps = 0, position = 0;
  while (length) {
    if (c < 2 || c >= cluster_count + 2 || ++steps > cluster_count) return 0;
    u32 off = data_offset + (c - 2) * cluster_bytes;
    if (off > ramdisk_bytes - cluster_bytes) return 0;
    if (offset < position + cluster_bytes) {
      u32 skip = offset - position, count = cluster_bytes - skip;
      if (count > length) count = length;
      memcpy(dest, RAMDISK + off + skip, count);
      dest += count; offset += count; length -= count;
    }
    position += cluster_bytes;
    u32 fat = fat_offset + c + c / 2;
    if (fat + 1 >= root_offset) return 0;
    u32 next = le16(RAMDISK + fat);
    c = (c & 1) ? next >> 4 : next & 0xfff;
  }
  return 1;
}
