#include "../api.h"
#include "../process.h"
#include "paging.h"
#include "uaccess.h"
#include "../lib/kstring.h"

/*
  pd: 页目录
  address: 虚拟地址
  bytes: 检查的字节数量
  access: 访问模式
  TODO: uaccess_prepare() 不能保证完全没有副作用,
  第一轮成功后，第二轮可能分配部分物理页，然后因内存不足而失败。此时需要确认上层如何处理已经建立的映射。
  TODO: uaccess_prepare() 的页面循环也值得检查边界行为
*/
int uaccess_prepare(u32* pd, u32 address, u32 bytes, UserAccess access) {
  u32 limit = process_user_limit(pd, address);
  if (!limit || bytes > limit - address) {
    return S_INVALID;
  }
  if (!bytes) {
    return S_OK;
  }

  // 计算需要的权限
  u32 needed =
      PAGE_PRESENT | PAGE_USER | (access == USER_WRITE ? PAGE_WRITE : 0);
  u32 last = address + bytes - 1;
  u32 first = page_floor(address);
  int missing = 0;
  for (u32 page = first; page <= last; page += PAGE_SIZE) {
    u32 flags = paging_user_flags(pd, page);
    if ((flags & needed) == needed) { //页面存在，权限OK
      continue;
    }
    if (flags & PAGE_PRESENT) { // 页面存在，权限不足
      return S_INVALID;
    }
    UACCESS_TRY(process_anon_page(pd, page, 0));
    missing = 1;
  }
  /* Check the whole range first, then populate before the syscall has effects.
   */
  if (missing) {
    for (u32 page = first; page <= last; page += PAGE_SIZE) {
      if (!(paging_user_flags(pd, page) & PAGE_PRESENT)) { // 先检查整个地址范围，再进行实际页面准备
        UACCESS_TRY(process_anon_page(pd, page, 1));
      }
    }
  }
  return S_OK;
}

// TODO: 为什么没有uaccess copy out
// 把用户进程虚拟地址空间中的一段数据复制到内核提供的缓冲区
int uaccess_copy_in(u32* pd, void* out, u32 address, u32 bytes) {
  UACCESS_TRY(uaccess_prepare(pd, address, bytes, USER_READ));
  if (bytes) {
    memcpy(out, (const void*)address, bytes);
  }
  return S_OK;
}

/* Prepare and scan one page at a time. Bytes after the NUL may be unmapped. */
int uaccess_copy_string(u32* pd, char* out, u32 address, u32 capacity) {
  while (capacity) {
    u32 span = PAGE_SIZE - page_offset(address);
    if (span > capacity) {
      span = capacity;
    }
    UACCESS_TRY(uaccess_prepare(pd, address, span, USER_READ));
    const char* source = (const char*)address;
    for (u32 i = 0; i < span; i++) {
      if (!(*out++ = source[i])) {
        return S_OK;
      }
    }
    capacity -= span;
    address += span;
  }
  return S_INVALID;
}
