/* Run the real storage driver against deterministic disk/VFS boundaries.
 * The JS runner only substitutes BIOS handoff addresses in its private copy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src32/types.h"
#define SINO_KERNEL_H
#define MAX_FILE 65536u
volatile u32 ticks;
int settings_language;
static u8 storage_test_boot_drive;
static u32 storage_test_boot_signature;
int streq(const char*, const char*);
int page_free(void*);
void number(u32, char*);
void pr_debug(const char*);
#include "storage-under-test.inc"

#define TEST_SECTORS 16400u
static u8 disk_bytes[TEST_SECTORS * 512u];
static u8 previous_bank[BANK_SECTORS * 512u];
static char diagnostic[8192];
static unsigned log_size, reads, writes, flushes;
static unsigned fail_read_at, fail_write_at, fail_flush_at;
static const char payload[] = "persistent payload";
Node nodes[VFS_NODES];
Disk disks[4];

int streq(const char* a, const char* b) {
  return strcmp(a, b) == 0;
}
int page_free(void* p) {
  (void)p;
  return 1;
}
void number(u32 v, char* out) {
  sprintf(out, "%u", v);
}
void pr_debug(const char* s) {
  size_t n = strlen(s);
  if (n >= sizeof(diagnostic) - log_size)
    abort();
  memcpy(diagnostic + log_size, s, n + 1);
  log_size += (unsigned)n;
}
int disk_read(int d, u32 lba, u8* out) {
  reads++;
  if (d != 0 || lba >= TEST_SECTORS || reads == fail_read_at)
    return 0;
  memcpy(out, disk_bytes + lba * 512u, 512);
  return 1;
}
int disk_write(int d, u32 lba, const u8* in) {
  writes++;
  if (d != 0 || lba >= TEST_SECTORS || writes == fail_write_at)
    return 0;
  memcpy(disk_bytes + lba * 512u, in, 512);
  return 1;
}
int disk_flush(int d) {
  flushes++;
  return d == 0 && flushes != fail_flush_at;
}

/* Controlled VFS fixture, not a second filesystem implementation. */
int vfs_lookup(const char* path) {
  return streq(path, "/home/user") ? 1 : -1;
}
void vfs_path(int index, char* out) {
  if (index != 2)
    abort();
  strcpy(out, "/home/user/note.txt");
}
int vfs_canonical(const char* in, char* out) {
  strcpy(out, in);
  return 0;
}
int vfs_create(const char* path, int dir, int parents) {
  (void)path;
  (void)dir;
  (void)parents;
  abort();
}
int vfs_read(int index, u8* out, u32 capacity) {
  if (index != 2 || capacity < sizeof(payload) - 1)
    abort();
  memcpy(out, payload, sizeof(payload) - 1);
  return sizeof(payload) - 1;
}
int vfs_write(int index, const u8* in, u32 n, int append) {
  (void)index;
  (void)in;
  (void)n;
  (void)append;
  abort();
}

static void check(const char* id, int ok) {
  printf("CHECK\t%s\t%d\n", id, !!ok);
}
static void reset(void) {
  memset(disk_bytes, 0, sizeof(disk_bytes));
  memset(nodes, 0, sizeof(nodes));
  memset(disks, 0, sizeof(disks));
  disks[0].present = 1;
  disks[0].sectors = TEST_SECTORS;
  nodes[1].used = nodes[1].dir = 1;
  reads = writes = flushes = fail_read_at = fail_write_at = fail_flush_at = 0;
  storage_ready = storage_dirty = 0;
  storage_generation = 0;
  storage_disk = current_bank = -1;
  storage_partition = volume_base = sectors = changed_at = ticks = 0;
  loading = 0;
  storage_error = "No data disk attached";
  storage_error_key = STORAGE_ERROR_NO_DATA_DISK_ATTACHED;
  reported_error = TEXT_NONE;
  log_size = 0;
  diagnostic[0] = 0;
}
static void healthy(void) {
  reset();
  storage_init();
  if (!storage_ready || storage_dirty || storage_generation != 1)
    abort();
}
static void note(void) {
  nodes[2].used = 1;
  nodes[2].parent = 1;
  nodes[2].size = sizeof(payload) - 1;
  storage_changed();
}
static int specific_reason(void) {
  return storage_error[0] && !streq(storage_error, "No data disk attached");
}

int main(void) {
  reset();
  disks[0].present = 0;
  storage_init();
  check("storage.no-disk",
        !storage_ready && !writes && !storage_sync() && storage_error[0]);
  healthy();
  check("storage.blank-init", inspect(current_bank).valid && !storage_error[0]);
  unsigned before_writes = writes;
  check("storage.clean-sync", storage_sync() == 1 && writes == before_writes);
  note();
  check("storage.commit", storage_sync() == 1 && storage_generation == 2 &&
                              !storage_dirty && !storage_error[0] &&
                              inspect(current_bank).valid);

  /* Every failed stage must preserve the previously committed bank exactly. */
  const char* ids[] = {"storage.header-failure", "storage.payload-failure",
                       "storage.payload-flush-failure",
                       "storage.commit-failure",
                       "storage.commit-flush-failure"};
  for (int mode = 0; mode < 5; mode++) {
    healthy();
    note();
    int old_bank = current_bank;
    u32 old_generation = storage_generation;
    memcpy(previous_bank, disk_bytes + BANK_START(old_bank) * 512u,
           sizeof(previous_bank));
    if (mode == 0)
      fail_write_at = writes + 1;
    if (mode == 1)
      fail_write_at = writes + 2;
    if (mode == 2)
      fail_flush_at = flushes + 1;
    if (mode == 3)
      fail_write_at = writes + 3;
    if (mode == 4)
      fail_flush_at = flushes + 2;
    int result = storage_sync();
    check(ids[mode],
          result == 0 && storage_ready && storage_dirty &&
              current_bank == old_bank &&
              storage_generation == old_generation && storage_error[0] &&
              inspect(old_bank).valid &&
              !memcmp(previous_bank, disk_bytes + BANK_START(old_bank) * 512u,
                      sizeof(previous_bank)));
    fail_write_at = fail_flush_at = 0;
    check(mode == 0   ? "storage.retry-clears-error"
          : mode == 1 ? "storage.retry-payload"
          : mode == 2 ? "storage.retry-flush"
          : mode == 3 ? "storage.retry-commit"
                      : "storage.retry-commit-flush",
          storage_sync() == 1 && !storage_error[0] && !storage_dirty &&
              storage_generation == old_generation + 1);
  }

  healthy();
  for (int i = 2; i < 72; i++) {
    nodes[i].used = 1;
    nodes[i].parent = 1;
    nodes[i].size = MAX_FILE;
  }
  storage_changed();
  before_writes = writes;
  check("storage.full-volume", storage_sync() == 0 && writes == before_writes &&
                                   storage_dirty && storage_error[0]);

  reset();
  fail_read_at = 2;
  storage_init();
  check("storage.volume-read-safe", !storage_ready && !writes);
  check("storage.volume-read-reason", specific_reason());
  reset();
  fail_write_at = 1;
  storage_init();
  check("storage.volume-create-write-safe", !storage_ready && writes == 1);
  check("storage.volume-create-write-reason", specific_reason());
  reset();
  fail_flush_at = 1;
  storage_init();
  check("storage.volume-create-flush-safe", !storage_ready && flushes == 1);
  check("storage.volume-create-flush-reason", specific_reason());

  reset();
  fail_write_at = 2;
  storage_init();
  check("storage.initial-snapshot-retryable", storage_ready && storage_dirty &&
                                                  !storage_generation &&
                                                  specific_reason());
  check("storage.initial-snapshot-no-success-log",
        !strstr(diagnostic, "initialized SINO data volume"));

  healthy();
  note();
  log_size = 0;
  diagnostic[0] = 0;
  before_writes = writes;
  ticks = 19;
  storage_poll();
  int not_early = writes == before_writes;
  fail_write_at = writes + 1;
  ticks = 20;
  storage_poll();
  unsigned after_failure = writes;
  unsigned first_failure_log_size = log_size;
  ticks = 21;
  storage_poll();
  int waits = writes == after_failure;
  fail_write_at = writes + 1;
  ticks = 40;
  storage_poll();
  check("storage.background-retry-spacing",
        not_early && waits && writes == after_failure + 1 && storage_dirty);
  check("storage.background-failure-log",
        first_failure_log_size > 0 && log_size == first_failure_log_size);
  return 0;
}
