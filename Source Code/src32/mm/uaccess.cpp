#include "../api.h"
#include "../task/task.h"
#include "paging.h"
#include "uaccess.h"
#include "vma.h"
#include "../lib/kstring.h"

int uaccess_prepare(u32* pd, u32 address, u32 bytes, UserAccess access) {
  const bool write = access == UserAccess::WRITE;
  if ((!write && access != UserAccess::READ) || address < USER_BASE || address >= USER_END ||
      bytes > USER_END - address) {
    return S_INVALID;
  }
  const auto* root = task::userAccessVmas(pd);
  if (!root || !paging_is_active(pd)) { return S_INVALID; }
  if (!bytes) { return S_OK; }

  using mm::VmaNode;
  using mm::VmaType;
  const u32 needed = PAGE_PRESENT | PAGE_USER | (write ? PAGE_WRITE : 0);
  const u32 first = address / PAGE_SIZE;
  const u32 end = (address + bytes - 1) / PAGE_SIZE + 1;
  u32 page = first;
  bool missing = false;
  for (auto* node = mm::vma_find(root, first); page < end; node = mm::vma_next(node)) {
    if (!node || node->startPage > page || node->type == VmaType::Guard ||
        !node->hasFlag(VmaNode::Flag::Read) || (write && !node->hasFlag(VmaNode::Flag::Write))) {
      return S_INVALID;
    }
    const u32 nodeEnd = node->startPage + node->pageCount;
    const u32 stop = nodeEnd < end ? nodeEnd : end;
    for (; page < stop; ++page) {
      const u32 flags = paging_user_flags(pd, page * PAGE_SIZE);
      if ((flags & needed) == needed) { continue; }
      if ((flags & PAGE_PRESENT) || node->type != VmaType::Anonymous) { return S_INVALID; }
      missing = true;
    }
  }
  /* OOM may retain zeroed anonymous pages already populated. They remain owned
   * by this process and are reclaimed by unmap/exit. No data copy or external
   * I/O has happened; callers must prepare before consuming/mutating objects. */
  if (missing) {
    for (page = first; page < end; ++page) {
      const u32 va = page * PAGE_SIZE;
      if (!(paging_user_flags(pd, va) & PAGE_PRESENT)) {
        UACCESS_TRY(process_populate_anon_page(pd, va, write));
      }
    }
  }
  return S_OK;
}

int copy_from_user(u32* pd, void* out, u32 address, u32 bytes) {
  UACCESS_TRY(uaccess_prepare(pd, address, bytes, UserAccess::READ));
  if (bytes) memcpy(out, (const void*)address, bytes);
  return S_OK;
}
int copy_to_user(u32* pd, u32 address, const void* source, u32 bytes) {
  UACCESS_TRY(uaccess_prepare(pd, address, bytes, UserAccess::WRITE));
  if (bytes) memcpy((void*)address, source, bytes);
  return S_OK;
}
/* Strings may update a prefix of out on failure. Only bytes up to the NUL
 * need to be accessible; the next page is never touched after termination. */
int copy_string_from_user(u32* pd, char* out, u32 address, u32 capacity) {
  while (capacity) {
    u32 span = PAGE_SIZE - page_offset(address);
    if (span > capacity) span = capacity;
    UACCESS_TRY(uaccess_prepare(pd, address, span, UserAccess::READ));
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
