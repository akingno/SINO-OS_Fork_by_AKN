#include "../api.h"
#include "../process.h"
#include "paging.h"
#include "uaccess.h"
#include "../lib/kstring.h"

int uaccess_prepare(u32* pd, u32 address, u32 bytes, UserAccess access) {
  if (access != USER_READ && access != USER_WRITE) return S_INVALID;
  u32 limit = process_user_limit(pd, address);
  if (!limit || address > limit || bytes > limit - address || !paging_is_active(pd))
    return S_INVALID;
  if (!bytes) return S_OK;
  u32 needed = PAGE_PRESENT | PAGE_USER | (access == USER_WRITE ? PAGE_WRITE : 0);
  u32 last_page = page_floor(address + bytes - 1);
  u32 first = page_floor(address);
  int missing = 0;
  /* Validation has no allocation or external effects. */
  for (u32 page = first;; page += PAGE_SIZE) {
    u32 flags = paging_user_flags(pd, page);
    if ((flags & needed) != needed) {
      if (flags & PAGE_PRESENT) return S_INVALID;
      UACCESS_TRY(process_anon_page(pd, page, 0));
      missing = 1;
    }
    if (page == last_page) break;
  }
  /* OOM may retain zeroed anonymous pages already populated. They remain owned
   * by this process and are reclaimed by unmap/exit. No data copy or external
   * I/O has happened; callers must prepare before consuming/mutating objects. */
  if (missing) {
    for (u32 page = first;; page += PAGE_SIZE) {
      if (!(paging_user_flags(pd, page) & PAGE_PRESENT))
        UACCESS_TRY(process_anon_page(pd, page, 1));
      if (page == last_page) break;
    }
  }
  return S_OK;
}

int copy_from_user(u32* pd, void* out, u32 address, u32 bytes) {
  UACCESS_TRY(uaccess_prepare(pd, address, bytes, USER_READ));
  if (bytes) memcpy(out, (const void*)address, bytes);
  return S_OK;
}
int copy_to_user(u32* pd, u32 address, const void* source, u32 bytes) {
  UACCESS_TRY(uaccess_prepare(pd, address, bytes, USER_WRITE));
  if (bytes) memcpy((void*)address, source, bytes);
  return S_OK;
}
/* Strings may update a prefix of out on failure. Only bytes up to the NUL
 * need to be accessible; the next page is never touched after termination. */
int copy_string_from_user(u32* pd, char* out, u32 address, u32 capacity) {
  while (capacity) {
    u32 span = PAGE_SIZE - page_offset(address);
    if (span > capacity) span = capacity;
    UACCESS_TRY(uaccess_prepare(pd, address, span, USER_READ));
    const char* source = (const char*)address;
    for (u32 i = 0; i < span; i++) {
      if (!(*out++ = source[i])) return S_OK;
    }
    capacity -= span;
    if (!capacity || address > 0xffffffffu - span) return S_INVALID;
    address += span;
  }
  return S_INVALID;
}
