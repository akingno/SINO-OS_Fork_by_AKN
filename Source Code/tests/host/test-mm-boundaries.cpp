#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef S_OK
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src32/api.h"
#include "../../src32/mm/e820.h"
#include "../../src32/mm/paging.h"
#include "../../src32/mm/uaccess.h"
#include "../../src32/mm/vma.h"
using mm::VmaNode;
using mm::VmaType;
#define BASE 0x41000000u
static u32 directory, foreign;
static int active = 1, budget, populated;
static u32 flags[3];
static VmaNode vmas[3];
static u8* memory;
namespace task {
const VmaNode* userAccessVmas(const u32* pd) { return pd == &directory ? vmas : nullptr; }
}
namespace mm {
const VmaNode* vma_find(const VmaNode* root, u32 page) {
  for (const auto* node = root; node != vmas + 3; ++node) {
    if (node->startPage == page && node->pageCount) { return node; }
  }
  return nullptr;
}
const VmaNode* vma_next(const VmaNode* node) {
  while (++node != vmas + 3) {
    if (node->pageCount) { return node; }
  }
  return nullptr;
}
}
static void region(unsigned index, VmaType type, bool writable = false) {
  auto& node = vmas[index];
  node = VmaNode{BASE / PAGE_SIZE + index, 1, 0, 0, 0, type};
  node.setFlag(VmaNode::Flag::Read, type != VmaType::Guard);
  node.setFlag(VmaNode::Flag::Write, writable);
}
extern "C" int paging_is_active(const u32* pd) { return active && pd == &directory; }
extern "C" u32 paging_user_flags(const u32* pd, u32 address) {
  if (pd != &directory || address < BASE || address >= BASE + 3 * PAGE_SIZE) return 0;
  return flags[(address - BASE) / PAGE_SIZE];
}
extern "C" int process_populate_anon_page(u32* pd, u32 address, bool write) {
  if (pd != &directory || address < BASE || address >= BASE + 3 * PAGE_SIZE) return S_INVALID;
  u32 i = (address - BASE) / PAGE_SIZE;
  const auto* node = mm::vma_find(task::userAccessVmas(pd), address / PAGE_SIZE);
  assert(node && node->type == VmaType::Anonymous && !(flags[i] & PAGE_PRESENT));
  const bool writable = node->hasFlag(VmaNode::Flag::Write);
  assert(!write || writable);
  if (!budget) return S_NOMEM;
  budget--; populated++;
  flags[i] = PAGE_PRESENT | PAGE_USER | (writable ? PAGE_WRITE : 0);
  memset(memory + i * PAGE_SIZE, 0, PAGE_SIZE);
  return S_OK;
}
static void reset(void) {
  active = 1; budget = 3; populated = 0;
  memset(flags, 0, sizeof(flags)); memset(vmas, 0, sizeof(vmas));
  memset(memory, 0x5a, 3 * PAGE_SIZE);
}
static BootRange range(u32 base, u32 length, u32 type) {
  E820Entry e = {base, 0, length, 0, type, 1}; BootRange r;
  assert(e820_range(&e, &r)); return r;
}
int main(void) {
  memory = (u8*)VirtualAlloc((void*)BASE, 3 * PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  assert(memory == (u8*)BASE);
  u32 rounded = 123;
  assert(page_ceil_checked(0, &rounded) && rounded == 0);
  assert(page_ceil_checked(1, &rounded) && rounded == PAGE_SIZE);
  assert(page_ceil_checked(0xfffff000u, &rounded) && rounded == 0xfffff000u);
  assert(!page_ceil_checked(0xfffff001u, &rounded) && rounded == 0xfffff000u);
  assert(!page_ceil_checked(0xffffffffu, &rounded) && !page_ceil_checked(0, 0));
  puts("PASS mm.boundaries.align-overflow");
  BootRange r = range(0x1001, 0x2ffe, 1);
  assert(r.start_pfn == 2 && r.end_pfn == 3);
  E820Entry e = {0x1001, 0, 1, 0, 1, 1};
  assert(!e820_range(&e, &r));
  puts("PASS mm.boundaries.usable-inward");
  r = range(0x1fff, 2, 2); assert(r.start_pfn == 1 && r.end_pfn == 3);
  r = range(0x2000, 1, 2); assert(r.start_pfn == 2 && r.end_pfn == 3);
  puts("PASS mm.boundaries.reserved-outward");
  e = (E820Entry){0xfffff000u, 0, 0x1000, 0, 1, 1};
  assert(e820_range(&e, &r) && r.end_pfn == PHYS_LIMIT_PAGES);
  e = (E820Entry){0x1000, 0, 0xffffffffu, 0xffffffffu, 1, 1};
  assert(e820_range(&e, &r) && r.start_pfn == 1 && r.end_pfn == PHYS_LIMIT_PAGES);
  e.base_hi = 1; assert(!e820_range(&e, &r));
  e.base_hi = 0; e.attributes = 0; assert(!e820_range(&e, &r));
  e.attributes = 1; e.length = e.length_hi = 0; assert(!e820_range(&e, &r));
  r = range(0xffffffffu, 1, 2); assert(r.start_pfn == 0xfffff && r.end_pfn == PHYS_LIMIT_PAGES);
  puts("PASS mm.boundaries.e820-wide-and-invalid");
  BootRange ranges[] = {range(0x2fff, 2, 2), range(0, 0x6000, 1), range(0x1000, 0x4000, 1)};
  assert(e820_page_usable(ranges, 3, 1));
  assert(!e820_page_usable(ranges, 3, 2) && !e820_page_usable(ranges, 3, 3));
  assert(e820_page_usable(ranges, 3, 4) && !e820_page_usable(ranges, 3, 6));
  puts("PASS mm.boundaries.reserved-overlap-priority");
  reset(); region(0, VmaType::Anonymous, true);
  assert(uaccess_prepare(&foreign, BASE, 1, UserAccess::READ) == S_INVALID);
  active = 0; assert(uaccess_prepare(&directory, BASE, 1, UserAccess::READ) == S_INVALID);
  assert(!populated);
  puts("PASS mm.boundaries.uaccess-address-space");
  reset();
  assert(uaccess_prepare(&directory, BASE, 0, UserAccess::READ) == S_OK);
  assert(uaccess_prepare(&directory, USER_END, 0, UserAccess::READ) == S_INVALID);
  assert(uaccess_prepare(&directory, BASE, 0, (UserAccess)99) == S_INVALID);
  assert(uaccess_prepare(&directory, 0xffffffffu, 2, UserAccess::READ) == S_INVALID);
  assert(uaccess_prepare(&directory, BASE + 3 * PAGE_SIZE, 1, UserAccess::READ) == S_INVALID);
  flags[0] = PAGE_PRESENT | PAGE_USER | PAGE_WRITE;
  assert(uaccess_prepare(&directory, BASE, 1, UserAccess::READ) == S_INVALID);
  region(0, VmaType::Image);
  assert(uaccess_prepare(&directory, BASE, 1, UserAccess::READ) == S_OK);
  assert(uaccess_prepare(&directory, BASE, 1, UserAccess::WRITE) == S_INVALID && !populated);
  region(0, VmaType::Stack, true);
  flags[0] = PAGE_PRESENT | PAGE_USER;
  assert(uaccess_prepare(&directory, BASE, 1, UserAccess::WRITE) == S_INVALID);
  flags[0] = PAGE_PRESENT | PAGE_WRITE;
  assert(uaccess_prepare(&directory, BASE, 1, UserAccess::READ) == S_INVALID);
  flags[0] = 0;
  assert(uaccess_prepare(&directory, BASE, 1, UserAccess::READ) == S_INVALID && !populated);
  puts("PASS mm.boundaries.uaccess-permissions-and-bounds");
  reset(); region(0, VmaType::Anonymous, true); char out[4] = "old";
  assert(copy_from_user(&directory, out, BASE + PAGE_SIZE - 2, 4) == S_INVALID);
  assert(!populated && !memcmp(out, "old", 4));
  assert(copy_to_user(&directory, BASE + PAGE_SIZE - 2, "abcd", 4) == S_INVALID);
  assert(!populated && memory[PAGE_SIZE - 2] == 0x5a);
  region(1, VmaType::Anonymous);
  assert(copy_to_user(&directory, BASE + PAGE_SIZE - 2, "abcd", 4) == S_INVALID && !populated);
  region(1, VmaType::Guard);
  flags[1] = PAGE_PRESENT | PAGE_USER | PAGE_WRITE;
  assert(copy_from_user(&directory, out, BASE + PAGE_SIZE - 2, 4) == S_INVALID && !populated);
  region(1, VmaType::Anonymous, true);
  flags[1] = PAGE_PRESENT | PAGE_USER;
  assert(copy_to_user(&directory, BASE + PAGE_SIZE - 2, "abcd", 4) == S_INVALID && !populated);
  puts("PASS mm.boundaries.validation-before-allocation");
  reset(); region(0, VmaType::Anonymous, true); region(1, VmaType::Anonymous, true); budget = 1;
  assert(copy_to_user(&directory, BASE + PAGE_SIZE - 2, "abcd", 4) == S_NOMEM);
  assert(populated == 1 && flags[0] && !flags[1] && memory[PAGE_SIZE - 2] == 0);
  assert(copy_to_user(&directory, BASE, "ok", 3) == S_OK && !memcmp(memory, "ok", 3));
  /* Model unmap returns the retained page; a fresh reservation starts zeroed. */
  flags[0] = 0; budget = 1;
  assert(uaccess_prepare(&directory, BASE, 1, UserAccess::READ) == S_OK && memory[0] == 0);
  puts("PASS mm.boundaries.partial-population-oom");
  reset(); flags[0] = flags[1] = PAGE_PRESENT | PAGE_USER | PAGE_WRITE;
  region(0, VmaType::Stack, true); region(1, VmaType::Guard);
  memory[PAGE_SIZE - 2] = 'a'; memory[PAGE_SIZE - 1] = 0;
  flags[1] = 0;
  assert(copy_string_from_user(&directory, out, BASE + PAGE_SIZE - 2, 4) == S_OK && !strcmp(out, "a"));
  memory[PAGE_SIZE - 1] = 'b'; out[0] = 0;
  assert(copy_string_from_user(&directory, out, BASE + PAGE_SIZE - 2, 4) == S_INVALID && out[0] == 'a' && out[1] == 'b');
  region(1, VmaType::Anonymous, true);
  flags[1] = PAGE_PRESENT | PAGE_USER | PAGE_WRITE;
  assert(copy_to_user(&directory, BASE + PAGE_SIZE - 2, "abc", 4) == S_OK);
  assert(copy_from_user(&directory, out, BASE + PAGE_SIZE - 2, 4) == S_OK && !strcmp(out, "abc"));
  assert(copy_string_from_user(&directory, out, BASE, 0) == S_INVALID);
  puts("PASS mm.boundaries.string-and-copy-roundtrip");
  assert(VirtualFree(memory, 0, MEM_RELEASE)); return 0;
}
