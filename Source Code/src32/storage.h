#ifndef SINO_STORAGE_H
#define SINO_STORAGE_H
#include "types.h"
#include "locale.h"
extern int storage_ready, storage_dirty;
extern int storage_disk;
extern u32 storage_partition;
extern u32 storage_generation;
extern const char* storage_error;
extern TextKey storage_error_key;
void storage_init(void);
void storage_changed(void);
void storage_poll(void);
int storage_sync(void);
/* True for existing descendants serialized into the user snapshot.
   /home/user itself is recreated by VFS initialization, not serialized. */
int storage_tracks_node(int);
#endif
