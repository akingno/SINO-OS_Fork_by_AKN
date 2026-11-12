#ifndef SINO_BOOT_LAYOUT_H
#define SINO_BOOT_LAYOUT_H
#ifdef __cplusplus
extern "C" {
#endif
/* Shared by C, the image builders and NASM command-line definitions. */
#define BOOT_SECTORS 5760
#define BOOT_TRACK_SECTORS 36
#define BOOT_CLUSTER_SECTORS 2
#define BOOT_RESERVED_SECTORS 321
#define BOOT_FAT_SECTORS 9
#define BOOT_ROOT_ENTRIES 224
#ifdef __cplusplus
}
#endif
#endif
