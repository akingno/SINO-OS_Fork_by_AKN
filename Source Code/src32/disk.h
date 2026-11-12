#ifndef SINO_DISK_H
#define SINO_DISK_H
#include "kernel.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  int present;
  u32 sectors;
  char model[41];
} Disk;
extern Disk disks[4];
void disk_init(void);
int disk_read(int, u32, u8 *);
int disk_write(int, u32, const u8 *);
int disk_flush(int);
#ifdef __cplusplus
}
#endif
#endif
