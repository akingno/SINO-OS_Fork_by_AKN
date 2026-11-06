#ifndef SINO_BOOT_FAT12_H
#define SINO_BOOT_FAT12_H
#include "../types.h"

typedef struct {
  char name[13];
  u32 size;
  u16 cluster;
  u16 reserved;
} File;

extern File files[224];
extern u32 file_count;
int fs_init(void);
int fs_read(u32, u8*, u32);
int fs_read_at(u32, u32, u8*, u32);

#endif
