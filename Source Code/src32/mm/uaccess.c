#include "../api.h"
#include "../process.h"
#include "paging.h"
#include "uaccess.h"
#include "../lib/kstring.h"

#define PAGE_BYTES 4096u

int uaccess_prepare(u32* pd, u32 address, u32 bytes, UserAccess access) {
  u32 limit = process_user_limit(pd, address);
  if (!limit || bytes > limit - address)
    return S_INVALID;
  if (!bytes)
    return S_OK;

  u32 needed = PAGE_PRESENT | PAGE_USER | (access == USER_WRITE ? PAGE_WRITE : 0);
  u32 last = address + bytes - 1;
  u32 first = address & ~(PAGE_BYTES - 1);
  int missing = 0;
  for (u32 page = first; page <= last; page += PAGE_BYTES) {
    u32 flags = paging_user_flags(pd, page);
    if ((flags & needed) == needed)
      continue;
    if (flags & PAGE_PRESENT)
      return S_INVALID;
    UACCESS_TRY(process_anon_page(pd, page, 0));
    missing = 1;
  }
  /* Check the whole range first, then populate before the syscall has effects. */
  if (missing)
    for (u32 page = first; page <= last; page += PAGE_BYTES)
      if (!(paging_user_flags(pd, page) & PAGE_PRESENT))
        UACCESS_TRY(process_anon_page(pd, page, 1));
  return S_OK;
}

int uaccess_copy_in(u32* pd, void* out, u32 address, u32 bytes) {
  UACCESS_TRY(uaccess_prepare(pd, address, bytes, USER_READ));
  if (bytes)
    memcpy(out, (const void*)address, bytes);
  return S_OK;
}

/* Prepare and scan one page at a time. Bytes after the NUL may be unmapped. */
int uaccess_copy_string(u32* pd, char* out, u32 address, u32 capacity) {
  while (capacity) {
    u32 span = PAGE_BYTES - (address & (PAGE_BYTES - 1));
    if (span > capacity)
      span = capacity;
    UACCESS_TRY(uaccess_prepare(pd, address, span, USER_READ));
    const char* source = (const char*)address;
    for (u32 i = 0; i < span; i++)
      if (!(*out++ = source[i]))
        return S_OK;
    capacity -= span;
    address += span;
  }
  return S_INVALID;
}
